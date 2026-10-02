/**
 * Copyright 2018 VMware
 * Copyright 2018 Ted Yin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cassert>
#include <stack>

#include "hotstuff/util.h"
#include "hotstuff/consensus.h"
#include "hotstuff/auncel.h"
#include "hotstuff/stage_profiling.h"

#define LOG_INFO HOTSTUFF_LOG_INFO
#define LOG_DEBUG HOTSTUFF_LOG_DEBUG
#define LOG_WARN HOTSTUFF_LOG_WARN
#define LOG_PROTO HOTSTUFF_LOG_PROTO

namespace hotstuff {

/* The core logic of HotStuff, is fairly simple :). */
/*** begin HotStuff protocol logic ***/
HotStuffCore::HotStuffCore(ReplicaID id,
                            privkey_bt &&priv_key):
        b0(new Block(true, 1)),
        b_lock(b0),
        b_exec(b0),
        vheight(0),
        priv_key(std::move(priv_key)),
        tails{b0},
        vote_disabled(false),
        id(id),
        storage(new EntityStorage()) {
    storage->add_blk(b0);
}

void HotStuffCore::sanity_check_delivered(const block_t &blk) {
    if (!blk->delivered)
        throw std::runtime_error("block not delivered");
}

block_t HotStuffCore::get_delivered_blk(const uint256_t &blk_hash) {
    block_t blk = storage->find_blk(blk_hash);
    if (blk == nullptr || !blk->delivered)
        throw std::runtime_error("block not delivered");
    return blk;
}

bool HotStuffCore::on_deliver_blk(const block_t &blk) {
    if (blk->delivered)
    {
        LOG_WARN("attempt to deliver a block twice");
        return false;
    }
    blk->parents.clear();
    for (const auto &hash: blk->parent_hashes)
        blk->parents.push_back(get_delivered_blk(hash));
    blk->height = blk->parents[0]->height + 1;

    if (blk->qc)
    {
        block_t _blk = storage->find_blk(blk->qc->get_obj_hash());
        if (_blk == nullptr)
            throw std::runtime_error("block referred by qc not fetched");
        blk->qc_ref = std::move(_blk);
    } // otherwise blk->qc_ref remains null

    for (auto pblk: blk->parents) tails.erase(pblk);
    tails.insert(blk);

    blk->delivered = true;
    LOG_DEBUG("deliver %s", std::string(*blk).c_str());
    return true;
}

void HotStuffCore::update_hqc(const block_t &_hqc, const quorum_cert_bt &qc) {
    if (_hqc->height > hqc.first->height)
    {
        hqc = std::make_pair(_hqc, qc->clone());
        on_hqc_update();
    }
}

void HotStuffCore::update(const block_t &nblk) {
    /* nblk = b*, blk2 = b'', blk1 = b', blk = b */
#ifndef HOTSTUFF_TWO_STEP
    /* three-step HotStuff */
    const block_t &blk2 = nblk->qc_ref;
    if (blk2 == nullptr) return;
    /* decided blk could possible be incomplete due to pruning */
    if (blk2->decision) return;
    update_hqc(blk2, nblk->qc);

    const block_t &blk1 = blk2->qc_ref;
    if (blk1 == nullptr) return;
    if (blk1->decision) return;
    if (blk1->height > b_lock->height) b_lock = blk1;

    const block_t &blk = blk1->qc_ref;
    if (blk == nullptr) return;
    if (blk->decision) return;

    /* commit requires direct parent */
    if (blk2->parents[0] != blk1 || blk1->parents[0] != blk) return;
#else
    /* two-step HotStuff */
    const block_t &blk1 = nblk->qc_ref;
    if (blk1 == nullptr) return;
    if (blk1->decision) return;
    update_hqc(blk1, nblk->qc);
    if (blk1->height > b_lock->height) b_lock = blk1;

    const block_t &blk = blk1->qc_ref;
    if (blk == nullptr) return;
    if (blk->decision) return;

    /* commit requires direct parent */
    if (blk1->parents[0] != blk) return;
#endif
    /* otherwise commit */
    std::vector<block_t> commit_queue;
    block_t b;
    for (b = blk; b->height > b_exec->height; b = b->parents[0])
    { /* TODO: also commit the uncles/aunts */
        commit_queue.push_back(b);
    }
    if (b != b_exec)
        throw std::runtime_error("safety breached :( " +
                                std::string(*blk) + " " +
                                std::string(*b_exec));
    auto _commit_t0 = STAGE_NOW();
    size_t _total_cmds = 0;
    for (auto it = commit_queue.rbegin(); it != commit_queue.rend(); it++)
    {
        const block_t &blk = *it;
        uint32_t blk_view = static_cast<uint32_t>(blk->height);
        pending_sort_votes.erase(blk_view);
        view_to_block.erase(blk_view);
        for (const auto& cmd : blk->get_cmds()) {
            missing_vote_count.erase(cmd);
            censorship_alerted.erase(cmd);
        }
        blk->decision = 1;
        do_consensus(blk);
        LOG_PROTO("commit %s", std::string(*blk).c_str());

        /* mark this block's commands as committed so they are
           skipped in future get_local_ordering() calls and
           pruned from receive_order_log */
        {
            std::lock_guard<std::mutex> lk(order_log_mutex);
            for (const auto& cmd : blk->get_cmds())
                committed_cmds.insert(cmd);
        }

        const auto& cmds = blk->get_cmds();
        std::vector<uint256_t> ordered_cmds = cmds;
        salticidae::ElapsedTime weight_timer;
        weight_timer.start();
        if (!blk->get_ordering_map().empty()) {
            /* CSC block: leader already finalized the ordering in
             * finalize_pending_proposal() via WeightOrderer::order(votes, k).
             * ordering_map[0] is the correct final ordering — do NOT re-run
             * WeightOrderer, which would double-apply final_order_phase
             * (recursive median split) and produce an incorrect result. */
            ordered_cmds = blk->get_ordering_map()[0];
        } else {
            /* Non-CSC / fallback block: compute local ordering and
             * apply WeightOrderer as the sole ordering authority. */
            std::unordered_set<uint256_t> cmd_set(cmds.begin(), cmds.end());
            auto local = get_local_ordering(cmd_set);
            if (!local.empty()) {
                std::vector<std::vector<uint256_t>> orderings = {local};
                auto _fair_t0 = STAGE_NOW();
                ordered_cmds = WeightOrderer::order(orderings, weight_k);
                STAGE_PROF("fairness", 0, ordered_cmds.size(), STAGE_US(_fair_t0));
            }
        }
        _total_cmds += ordered_cmds.size();
        weight_timer.stop();
        HOTSTUFF_LOG_INFO("[perf] weight_orderer: txns=%zu total_ms=%.3f",
                          cmds.size(), weight_timer.elapsed_sec * 1000);

        salticidae::ElapsedTime decide_timer;
        decide_timer.start();
        for (size_t i = 0; i < ordered_cmds.size(); i++)
            do_decide(Finality(id, 1, i, blk->height,
                                ordered_cmds[i], blk->get_hash()));
        decide_timer.stop();
        HOTSTUFF_LOG_INFO("[perf] final_order: txns=%zu total_ms=%.3f",
                          ordered_cmds.size(), decide_timer.elapsed_sec * 1000);
    }
    STAGE_PROF("commit_exec", commit_queue.empty() ? 0 : commit_queue.front()->height, _total_cmds, STAGE_US(_commit_t0));
    compact_receive_order_log();
    b_exec = blk;
}

void HotStuffCore::on_tx_sync(uint32_t view, const std::vector<uint256_t> &cur_txs,
                              ReplicaID leader_id, const uint256_t &leader_block_hash) {
    salticidae::ElapsedTime tx_sync_timer;
    tx_sync_timer.start();
    std::unordered_set<uint256_t> tx_set(cur_txs.begin(), cur_txs.end());
    auto local_ordering = get_local_ordering(tx_set);

    std::unordered_map<uint256_t, uint32_t> tx_to_idx;
    tx_to_idx.reserve(cur_txs.size());
    for (uint32_t i = 0; i < cur_txs.size(); i++)
        tx_to_idx[cur_txs[i]] = i;
    std::vector<uint32_t> ordering_ids;
    ordering_ids.reserve(local_ordering.size());
    for (const auto &tx : local_ordering) {
        auto it = tx_to_idx.find(tx);
        ordering_ids.push_back(it != tx_to_idx.end() ? it->second : UINT32_MAX);
    }

    std::vector<uint256_t> missing_txs;
    if (enable_censorship_detection) {
        {
            std::lock_guard<std::mutex> lk(order_log_mutex);
            for (const auto& cmd : receive_order_log) {
                if (!tx_set.count(cmd)
                    && std::find(missing_txs.begin(), missing_txs.end(), cmd) == missing_txs.end()) {
                    missing_txs.push_back(cmd);
                }
            }
        }
        if (!missing_txs.empty()) {
            HOTSTUFF_LOG_INFO("[[AUNCEL]] R-%d detected %zu missing txs in view=%u",
                              get_id(), missing_txs.size(), view);
        }
    }
    tx_sync_timer.stop();
    HOTSTUFF_LOG_INFO("[perf] tx_sync: view=%u txns=%zu total_ms=%.3f",
                      view, cur_txs.size(), tx_sync_timer.elapsed_sec * 1000);
    do_send_sort_vote(leader_id, local_ordering, ordering_ids, missing_txs, view, leader_block_hash);
}

void HotStuffCore::on_sort_vote(ReplicaID replica, uint32_t view,
                                const std::vector<uint256_t> &local_ordering,
                                const std::vector<uint32_t> &ordering_ids,
                                const std::vector<uint256_t> &missing_txs,
                                part_cert_bt &&cert) {
    auto &votes = pending_sort_votes[view];
    auto &certs = pending_sort_certs[view];
    votes.push_back(local_ordering);
    if (cert) {
        auto it = view_to_block.find(view);
        if (it != view_to_block.end()) {
            auto vote_hash = compute_vote_hash(it->second, local_ordering);
            if (cert->get_obj_hash() != vote_hash) {
                HOTSTUFF_LOG_WARN("[[AUNCEL]] Sort-Vote cert hash mismatch from R-%d (view=%u)", replica, view);
            } else if (!cert->verify(config.get_pubkey(replica))) {
                HOTSTUFF_LOG_WARN("[[AUNCEL]] Sort-Vote cert verify failed from R-%d (view=%u)", replica, view);
            } else {
                certs[replica] = std::move(cert);
            }
        } else {
            certs[replica] = std::move(cert);
        }
    }
    HOTSTUFF_LOG_DEBUG("[[AUNCEL]] on_sort_vote R-%d view=%u count=%zu missing=%zu",
                       replica, view, votes.size(), missing_txs.size());

    for (const auto& tx : missing_txs) {
        size_t new_count = ++missing_vote_count[tx];
        size_t threshold = static_cast<size_t>(config.nreplicas - config.nmajority + 1);
        if (new_count >= threshold && !censorship_alerted.count(tx)) {
            censorship_alerted.insert(tx);
            HOTSTUFF_LOG_INFO("[[AUNCEL CENSORSHIP]] tx %.10s missing from %zu sort-votes (threshold f+1=%zu)",
                              get_hex(tx).c_str(), new_count, threshold);
        }
    }

    size_t nreplicas = config.nreplicas;
    size_t nfaulty = nreplicas > config.nmajority ? nreplicas - config.nmajority : 1;
    size_t need = nreplicas > nfaulty ? nreplicas - nfaulty : 3;

    auto it = view_to_block.find(view);
    if (it != view_to_block.end()) {
        auto blk_it = storage->find_blk(it->second);
        if (blk_it != nullptr) {
            if (votes.size() >= need && blk_it->get_ordering_map().empty()) {
                std::vector<std::vector<uint256_t>> orderings = votes;
                blk_it->set_ordering_map(std::move(orderings));
                HOTSTUFF_LOG_INFO("[[AUNCEL]] block %.10s aggregated %zu sort-votes",
                                  get_hex(blk_it->get_hash()).c_str(), votes.size());
            }
        }
    }

    if (pending_proposal && pending_view == view && votes.size() >= need) {
        finalize_pending_proposal();
    }
}

void HotStuffCore::finalize_pending_proposal() {
    if (!pending_proposal) return;

    auto bnew = pending_proposal;
    uint32_t view = pending_view;

    auto &votes = pending_sort_votes[view];
    auto &certs = pending_sort_certs[view];

    auto _fair_t0 = STAGE_NOW();
    auto final_ordering = WeightOrderer::order(votes, weight_k);
    STAGE_PROF("fairness", 0, final_ordering.size(), STAGE_US(_fair_t0));
    std::vector<std::vector<uint256_t>> final_map;
    final_map.push_back(std::move(final_ordering));
    bnew->set_ordering_map(std::move(final_map));

    quorum_cert_bt oqc = create_quorum_cert(bnew->get_hash());
    size_t qc_added = 0;
    for (auto &kv : certs) {
        if (kv.second && kv.second->get_obj_hash() == bnew->get_hash()) {
            oqc->add_part(kv.first, *kv.second);
            qc_added++;
        }
    }
    if (qc_added >= 1) {
        oqc->compute();
        bnew->set_order_qc(std::move(oqc));
    }

    HOTSTUFF_LOG_INFO("[[AUNCEL CSC]] Leader finalized pending proposal block=%.10s view=%u orderings=%zu",
                      get_hex(bnew->get_hash()).c_str(), view, votes.size());

    Proposal prop(id, bnew, nullptr);
    prop.ordering_map = bnew->get_ordering_map();
    for (auto &kv : certs) {
        prop.vote_replica_ids.push_back(kv.first);
    }
    prop.vote_orderings = votes;
    for (auto &kv : certs) {
        prop.vote_certs.push_back(kv.second ? kv.second->clone() : nullptr);
    }
    on_receive_proposal(prop);
    on_propose_(prop);
    update(bnew);
    do_broadcast_proposal(prop);

    pending_proposal = nullptr;
    pending_sort_votes.erase(view);
    pending_sort_certs.erase(view);
}

block_t HotStuffCore::on_propose(const std::vector<uint256_t> &cmds,
                            const std::vector<block_t> &parents,
                            bytearray_t &&extra) {
    auto _propose_t0 = STAGE_NOW();
    if (parents.empty())
        throw std::runtime_error("empty parents");
    for (const auto &_: parents) tails.erase(_);
    /* create the new block with empty ordering (CSC: filled after Sort-Vote) */
    block_t bnew = storage->add_blk(
        new Block(parents, cmds,
            hqc.second->clone(), std::move(extra),
            parents[0]->height + 1,
            hqc.first,
            nullptr
        ));
    const uint256_t bnew_hash = bnew->get_hash();
    bnew->self_qc = create_quorum_cert(bnew_hash);
    on_deliver_blk(bnew);
    LOG_PROTO("propose %s", std::string(*bnew).c_str());
    if (bnew->height <= vheight)
        throw std::runtime_error("new block should be higher than vheight");

    uint32_t current_view = static_cast<uint32_t>(bnew->height);
    view_to_block[current_view] = bnew_hash;

    pending_proposal = bnew;
    pending_view = current_view;

    HOTSTUFF_LOG_INFO("[[AUNCEL CSC]] Leader created pending proposal block=%.10s view=%u, waiting for Sort-Votes",
                      get_hex(bnew_hash).c_str(), current_view);

    do_send_tx_sync(current_view, cmds, id, bnew_hash);
    STAGE_PROF("propose_gen", bnew->get_height(), 0, STAGE_US(_propose_t0));
    return bnew;
}

void HotStuffCore::on_receive_proposal(const Proposal &prop) {
    auto _vd_t0 = STAGE_NOW();
    LOG_PROTO("got %s", std::string(prop).c_str());
    bool self_prop = prop.proposer == get_id();
    block_t bnew = prop.blk;
    if (!self_prop)
    {
        sanity_check_delivered(bnew);
        update(bnew);
        if (!prop.ordering_map.empty() && bnew->get_ordering_map().empty())
            bnew->set_ordering_map(std::vector<std::vector<uint256_t>>(prop.ordering_map));
        if (bnew->has_order_qc()) {
            const auto &oqc = bnew->get_order_qc();
            if (oqc->get_obj_hash() != bnew->get_hash()) {
                HOTSTUFF_LOG_WARN("[[AUNCEL CSC]] OrderQC hash mismatch on block %.10s",
                                  get_hex(bnew->get_hash()).c_str());
            } else if (!oqc->verify(config)) {
                HOTSTUFF_LOG_WARN("[[AUNCEL CSC]] OrderQC verification failed on block %.10s",
                                  get_hex(bnew->get_hash()).c_str());
            } else {
                HOTSTUFF_LOG_INFO("[[AUNCEL CSC]] OrderQC verified for block %.10s",
                                  get_hex(bnew->get_hash()).c_str());
            }
        }
        size_t verified = 0;
        for (size_t i = 0; i < prop.vote_replica_ids.size() && i < prop.vote_orderings.size(); i++) {
            if (i < prop.vote_certs.size() && prop.vote_certs[i]) {
                auto expected = compute_vote_hash(bnew->get_hash(), prop.vote_orderings[i]);
                if (prop.vote_certs[i]->get_obj_hash() == expected &&
                    prop.vote_certs[i]->verify(config.get_pubkey(prop.vote_replica_ids[i]))) {
                    verified++;
                } else {
                    HOTSTUFF_LOG_WARN("[[AUNCEL CSC]] vote cert verify failed for R-%d on block %.10s",
                                      prop.vote_replica_ids[i], get_hex(bnew->get_hash()).c_str());
                }
            }
        }
        size_t nfaulty = config.nreplicas > config.nmajority ? config.nreplicas - config.nmajority : 1;
        size_t need = config.nreplicas > nfaulty ? config.nreplicas - nfaulty : 3;
        if (verified >= need) {
            HOTSTUFF_LOG_INFO("[[AUNCEL CSC]] %zu/%zu vote certs verified for block %.10s",
                              verified, need, get_hex(bnew->get_hash()).c_str());
        } else if (verified > 0) {
            HOTSTUFF_LOG_WARN("[[AUNCEL CSC]] only %zu/%zu vote certs verified for block %.10s",
                              verified, need, get_hex(bnew->get_hash()).c_str());
        }
    }
    bool opinion = false;
    if (bnew->height > vheight)
    {
        if (bnew->qc_ref && bnew->qc_ref->height > b_lock->height)
        {
            opinion = true; // liveness condition
            vheight = bnew->height;
        }
        else
        {   // safety condition (extend the locked branch)
            block_t b;
            for (b = bnew;
                b->height > b_lock->height;
                b = b->parents[0]);
            if (b == b_lock) /* on the same branch */
            {
                opinion = true;
                vheight = bnew->height;
            }
        }
    }
    LOG_PROTO("now state: %s", std::string(*this).c_str());
    if (!self_prop && bnew->qc_ref)
        on_qc_finish(bnew->qc_ref);
    on_receive_proposal_(prop);
    if (opinion && !vote_disabled)
        do_vote(prop.proposer,
            Vote(id, bnew->get_hash(),
                create_part_cert(*priv_key, bnew->get_hash()), this));
    STAGE_PROF("vote_decision", prop.blk->get_height(), prop.blk->get_cmds().size(), STAGE_US(_vd_t0));
}

void HotStuffCore::on_receive_vote(const Vote &vote) {
    LOG_PROTO("got %s", std::string(vote).c_str());
    LOG_PROTO("now state: %s", std::string(*this).c_str());
    block_t blk = get_delivered_blk(vote.blk_hash);
    assert(vote.cert);
    size_t qsize = blk->voted.size();
    if (qsize >= config.nmajority) return;
    if (!blk->voted.insert(vote.voter).second)
    {
        LOG_WARN("duplicate vote for %s from %d", get_hex10(vote.blk_hash).c_str(), vote.voter);
        return;
    }
    auto &qc = blk->self_qc;
    if (qc == nullptr)
    {
        LOG_WARN("vote for block not proposed by itself");
        qc = create_quorum_cert(blk->get_hash());
    }
    qc->add_part(vote.voter, *vote.cert);
    if (qsize + 1 == config.nmajority)
    {
        auto _vq_t0 = STAGE_NOW();
        qc->compute();
        update_hqc(blk, qc);
        on_qc_finish(blk);
        STAGE_PROF("vote_qc", blk->get_height(), 0, STAGE_US(_vq_t0));
    }
}
/*** end HotStuff protocol logic ***/
void HotStuffCore::on_init(uint32_t nfaulty) {
    config.nmajority = config.nreplicas - nfaulty;

    /* Paper §IV: fairness parameter γ requires n >= ceil(4f / (2γ - 1)) + 1.
     * If γ <= 0.5 the denominator is <= 0 and the constraint is vacuous. */
    if (weight_k > 0.5) {
        size_t n = config.nreplicas;
        double denom = 2.0 * weight_k - 1.0;
        size_t min_n = static_cast<size_t>(std::ceil(4.0 * nfaulty / denom)) + 1;
        if (n < min_n) {
            HOTSTUFF_LOG_WARN("[[AUNCEL]] gamma=%.2f requires n>=%zu but n=%zu — fairness guarantee may not hold",
                              weight_k, min_n, n);
        } else {
            HOTSTUFF_LOG_INFO("[[AUNCEL]] gamma=%.2f threshold satisfied: n=%zu >= %zu (f=%u)",
                              weight_k, n, min_n, nfaulty);
        }
    }

    b0->qc = create_quorum_cert(b0->get_hash());
    b0->qc->compute();
    b0->self_qc = b0->qc->clone();
    b0->qc_ref = b0;
    hqc = std::make_pair(b0, b0->qc->clone());
}

void HotStuffCore::prune(uint32_t staleness) {
    block_t start;
    /* skip the blocks */
    for (start = b_exec; staleness; staleness--, start = start->parents[0])
        if (!start->parents.size()) return;
    std::stack<block_t> s;
    start->qc_ref = nullptr;
    s.push(start);
    while (!s.empty())
    {
        auto &blk = s.top();
        if (blk->parents.empty())
        {
            storage->try_release_blk(blk);
            s.pop();
            continue;
        }
        blk->qc_ref = nullptr;
        s.push(blk->parents.back());
        blk->parents.pop_back();
    }
}

void HotStuffCore::add_replica(ReplicaID rid, const PeerId &peer_id,
                                pubkey_bt &&pub_key) {
    config.add_replica(rid,
            ReplicaInfo(rid, peer_id, std::move(pub_key)));
    b0->voted.insert(rid);
}

promise_t HotStuffCore::async_qc_finish(const block_t &blk) {
    if (blk->voted.size() >= config.nmajority)
        return promise_t([](promise_t &pm) {
            pm.resolve();
        });
    auto it = qc_waiting.find(blk);
    if (it == qc_waiting.end())
        it = qc_waiting.insert(std::make_pair(blk, promise_t())).first;
    return it->second;
}

void HotStuffCore::on_qc_finish(const block_t &blk) {
    auto it = qc_waiting.find(blk);
    if (it != qc_waiting.end())
    {
        it->second.resolve();
        qc_waiting.erase(it);
    }
}

promise_t HotStuffCore::async_wait_proposal() {
    return propose_waiting.then([](const Proposal &prop) {
        return prop;
    });
}

promise_t HotStuffCore::async_wait_receive_proposal() {
    return receive_proposal_waiting.then([](const Proposal &prop) {
        return prop;
    });
}

promise_t HotStuffCore::async_hqc_update() {
    return hqc_update_waiting.then([this]() {
        return hqc.first;
    });
}

void HotStuffCore::on_propose_(const Proposal &prop) {
    auto t = std::move(propose_waiting);
    propose_waiting = promise_t();
    t.resolve(prop);
}

void HotStuffCore::on_receive_proposal_(const Proposal &prop) {
    auto t = std::move(receive_proposal_waiting);
    receive_proposal_waiting = promise_t();
    t.resolve(prop);
}

void HotStuffCore::on_hqc_update() {
    auto t = std::move(hqc_update_waiting);
    hqc_update_waiting = promise_t();
    t.resolve();
}

HotStuffCore::operator std::string () const {
    DataStream s;
    s << "<hotstuff "
      << "hqc=" << get_hex10(hqc.first->get_hash()) << " "
      << "hqc.height=" << std::to_string(hqc.first->height) << " "
      << "b_lock=" << get_hex10(b_lock->get_hash()) << " "
      << "b_exec=" << get_hex10(b_exec->get_hash()) << " "
      << "vheight=" << std::to_string(vheight) << " "
      << "tails=" << std::to_string(tails.size()) << ">";
    return s;
}

}
