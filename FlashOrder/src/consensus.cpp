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
#include <chrono>
#include <cstdlib>
#include <stack>
#include <string>
#include <queue>
#include <algorithm>

#include "hotstuff/util.h"
#include "hotstuff/consensus.h"
#include "hotstuff/flashorder.h"
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

// FlashOrder: update() 简化 —— 不再有 graph 变异、missing edge 等操作
void HotStuffCore::update(const block_t &nblk) {
    /* FlashOrder: 标记 propose level 中已 propose 的交易 */
    for (const auto &cmd : nblk->get_cmds()) {
        storage->remove_local_order_seen_propose_level(cmd);
    }

    /* nblk = b*, blk2 = b'', blk1 = b', blk = b */
    HOTSTUFF_LOG_DEBUG("[[update Start]] [R-%d] [L-] new block = %.10s", get_id(), get_hex(nblk->get_hash()).c_str());
#ifndef HOTSTUFF_TWO_STEP
    /* three-step HotStuff */
    const block_t &blk2 = nblk->qc_ref;
    if (blk2 == nullptr) return;
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
    /* b0 - - - - -> blk -> blk1 -> blk2 */
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

    HOTSTUFF_LOG_DEBUG("[[update]] [R-%d] [L-] Commit queue Size = %lu", get_id(), commit_queue.size());
    auto _commit_t0 = STAGE_NOW();
    size_t _commit_cmds = 0;
    for (auto it = commit_queue.rbegin(); it != commit_queue.rend(); it++)
    {
        const block_t &blk = *it;

        // FlashOrder: fair_finalize 直接返回 block 中存储的线性排序
        const auto &order = fair_finalize(blk);
        _commit_cmds += order.size();
        HOTSTUFF_LOG_DEBUG("[[update]] [R-%d] [L-] Final Order Size = %lu", get_id(), order.size());

        blk->decision = 1;
        do_consensus(blk);
        LOG_PROTO("commit %s", std::string(*blk).c_str());
        size_t n = order.size();
        for (size_t i = 0; i < n; i++) {
            do_decide(Finality(id, 1, i, blk->height, order[i], blk->get_hash()));
            storage->remove_local_order_seen_execute_level(order[i]);
            storage->remove_from_proposed_cmds_cache(order[i]);
        }
        b_exec = blk;

        HOTSTUFF_LOG_DEBUG("[[update Decided]] [R-%d] [L-]", get_id());
    }
    STAGE_PROF("commit_exec", 0, _commit_cmds, STAGE_US(_commit_t0));
    HOTSTUFF_LOG_DEBUG("[[update Ends]] [R-%d] [L-]", get_id());
}

// FlashOrder: fair_finalize 直接返回 block 的 cmds（一轮完成，无需检查 tournament graph）
std::vector<uint256_t> HotStuffCore::fair_finalize(block_t const &blk) {
    return blk->get_cmds();
}

// FlashOrder: flashorder_propose — 执行一轮排序，收集所有副本的 OrderedList 并生成最终排序
std::vector<uint256_t> HotStuffCore::flashorder_propose() {
    HOTSTUFF_LOG_INFO("[[flashorder_propose]] [R-%d] START", get_id());
    /* [R2-D5] leader round period = ordered-list collection + ordering + consensus */
    static std::chrono::steady_clock::time_point _last_fair_ts = std::chrono::steady_clock::now();
    auto _round_now = std::chrono::steady_clock::now();
    STAGE_PROF("round_interval", 0, 0, (double)std::chrono::duration_cast<std::chrono::microseconds>(_round_now - _last_fair_ts).count());
    _last_fair_ts = _round_now;
    auto propose_start_ts = std::chrono::steady_clock::now();
    
    // 收集所有副本的 OrderedList (cmds + timestamps)
    std::vector<OrderedList> ordered_lists = storage->collect_ordered_lists();
    
    if (ordered_lists.empty()) {
        HOTSTUFF_LOG_INFO("[[flashorder_propose]] [R-%d] No ordered lists collected", get_id());
        return std::vector<uint256_t>();
    }

    // [RECV_ROW] dump the actual receive-order rows (per-replica cmds hashes) for
    // Gate-B replay verification.  Guarded by FO_DBG_RECV_ROWS.
    if (std::getenv("FO_DBG_RECV_ROWS")) {
        HOTSTUFF_LOG_INFO("[RECV_ROW] n_rows=%zu", ordered_lists.size());
        for (size_t r = 0; r < ordered_lists.size(); r++) {
            const size_t n = ordered_lists[r].cmds.size();
            const size_t np = (n + 14) / 15;
            for (size_t off = 0, part = 0; off < n; off += 15, part++) {
                std::string chunk;
                for (size_t t = off; t < std::min(n, off + 15); t++) {
                    if (t != off) chunk += ",";
                    chunk += get_hex(ordered_lists[r].cmds[t]);
                }
                HOTSTUFF_LOG_INFO("[RECV_ROW] R=%zu n=%zu part=%zu/%zu %s",
                                  r, n, part, np, chunk.c_str());
            }
        }
    }

    FlashOrder::HyperGraph<10000> hg;
    size_t nfaulty = config.nreplicas - config.nmajority;
    hg.set_k_d(config.preference_disagreement_k);
    std::vector<uint256_t> final_order = hg.finalize(ordered_lists, nfaulty,
                                                      config.nreplicas,
                                                      config.fairness_parameter);
    
    HOTSTUFF_LOG_INFO("[[flashorder_propose]] [R-%d] Final order size = %lu", get_id(), final_order.size());
    // witness/envelope debug: dump final order hashes
    if (final_order.size() <= 20 || std::getenv("FO_DBG_FULL")) {
        std::string ord, full;
        for (const auto &c : final_order) {
            ord  += (ord.empty()  ? "" : ",") + get_hex10(c);
            full += (full.empty() ? "" : ",") + get_hex(c);
        }
        HOTSTUFF_LOG_INFO("[[flashorder_propose]] [R-%d] FINAL_ORDER_DBG %s", get_id(), ord.c_str());
        if (std::getenv("FO_DBG_FULL")) {
            const size_t n = final_order.size();
            for (size_t off = 0, part = 0; off < n; off += 15, part++) {
                std::string fchunk;
                for (size_t t = off; t < std::min(n, off + 15); t++)
                    fchunk += (t == off ? "" : ",") + get_hex(final_order[t]);
                HOTSTUFF_LOG_INFO("[FINAL_ORDER_FULL] part=%zu/%zu %s",
                                  part, (n + 14) / 15, fchunk.c_str());
            }
        }
    }
    
    // 清空本轮已处理的 OrderedList 缓存
    storage->pop_all_front_ordered_lists();
    
    // 标记提议中的交易
    for (const auto &cmd : final_order) {
        storage->add_to_proposed_cmds_cache(cmd);
    }
    
    auto propose_end_ts = std::chrono::steady_clock::now();
    double total_ms = std::chrono::duration_cast<std::chrono::microseconds>(
        propose_end_ts - propose_start_ts).count() / 1000.0;
    LOG_INFO("[perf] flashorder_propose: txns=%zu total_ms=%.3f",
             final_order.size(), total_ms);
    
    return final_order;
}

block_t HotStuffCore::on_propose(const std::vector<uint256_t> &cmds,            // FlashOrder
                            const std::vector<block_t> &parents,
                            bytearray_t &&extra) {
    auto _propose_t0 = STAGE_NOW();
    if (parents.empty())
        throw std::runtime_error("empty parents");
    for (const auto &_: parents) tails.erase(_);

    /* create the new block */
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
     update(bnew);  // [STRICT] update before broadcast for fair comparison
     Proposal prop(id, bnew, nullptr);
     LOG_PROTO("propose %s", std::string(*bnew).c_str());
     if (bnew->height <= vheight)
         throw std::runtime_error("new block should be higher than vheight");
     /* self-receive the proposal (no need to send it through the network) */
     on_receive_proposal(prop);
     on_propose_(prop);
     /* broadcast to other replicas */
     do_broadcast_proposal(prop);
     STAGE_PROF("propose_gen", bnew->get_height(), cmds.size(), STAGE_US(_propose_t0));
     return bnew;
}

void HotStuffCore::on_receive_proposal(const Proposal &prop) {
    auto _vd_t0 = STAGE_NOW();
    LOG_PROTO("got %s", std::string(prop).c_str());
    bool self_prop = prop.proposer == get_id();
    block_t bnew = prop.blk;
    HOTSTUFF_LOG_DEBUG("[[on_receive_proposal]] [R-%d] [L-%d] block Received = %.10s, cmds=%lu",
        get_id(), prop.proposer, get_hex(bnew->get_hash()).c_str(), bnew->get_cmds().size());

    if (!self_prop)
    {
        sanity_check_delivered(bnew);
        update(bnew);
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
    if (opinion && !vote_disabled){
        do_vote(prop.proposer,
            Vote(id, bnew->get_hash(),
                create_part_cert(*priv_key, bnew->get_hash()), this));
    }
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
        qc->compute();
        update_hqc(blk, qc);
        on_qc_finish(blk);
    }
}

// FlashOrder: on_local_order — 副本构建 LocalOrder 并发送给 Leader
void HotStuffCore::on_local_order(ReplicaID proposer, const std::vector<uint256_t> &order,
                                   const std::vector<uint64_t> &timestamps) {
    HOTSTUFF_LOG_DEBUG("[[on_local_order]] [R-%d] [L-%d] START, cmds=%lu", get_id(), proposer, order.size());

    if (order.empty()) {
        HOTSTUFF_LOG_DEBUG("[[on_local_order]] [R-%d] [L-%d] Nothing to order", get_id(), proposer);
        return;
    }

    /** update seen **/
    storage->update_local_order_seen(order);

    /** e2e Condorcet 攻击注入: FLASHORDER_P_ADV=condorcet 时，按 replica_id 伪随机
     *  选出的 f 个拜占庭副本，将本地序热点窗口旋转 ±W/3，制造系统性双向偏好环。 **/
    if (const char *adv = std::getenv("FLASHORDER_P_ADV")) {
        std::string adv_mode(adv);
        if (adv_mode == "condorcet" && order.size() >= 8) {
            double frac = 0.29;
            if (const char *fv = std::getenv("FLASHORDER_P_ADV_FRAC")) {
                double p = std::atof(fv);
                if (p > 0 && p < 1) frac = p;
            }
            // 确定性指派：与视图无关，同一副本始终拜占庭
            uint32_t h = static_cast<uint32_t>(get_id()) * 2654435761u;
            bool byzantine = (h % 1000u) < static_cast<uint32_t>(frac * 1000);
            if (byzantine) {
                size_t n = order.size();
                size_t W = std::max<size_t>(4, n / 4);
                size_t lo = (n - W) / 2;
                size_t k = std::max<size_t>(1, W / 3);
                if (h & 0x100) k = W - k;  // 一半前旋、一半后旋 → 环
                std::vector<uint256_t> w(order.begin() + lo, order.begin() + lo + W);
                std::rotate(w.begin(), w.begin() + k, w.end());
                std::vector<uint256_t> attacked(order);
                std::copy(w.begin(), w.end(), attacked.begin() + lo);
                std::vector<uint64_t> ts(timestamps);
                std::vector<uint64_t> wts(ts.begin() + lo, ts.begin() + lo + W);
                std::rotate(wts.begin(), wts.begin() + k, wts.end());
                std::copy(wts.begin(), wts.end(), ts.begin() + lo);
                HOTSTUFF_LOG_INFO("[[ADV-condorcet]] [R-%d] rotating window [%zu,%zu) by %zu",
                                  get_id(), lo, lo + W, k);
                LocalOrder local_order_adv = LocalOrder(get_id(), attacked, ts, this);
                do_send_local_order(proposer, local_order_adv);
                return;
            }
        }
    }

    /** create LocalOrder struct Object **/
    LocalOrder local_order = LocalOrder(get_id(), order, timestamps, this);

    /** send local order to leader **/
    do_send_local_order(proposer, local_order);
}

// FlashOrder: on_receive_local_order — Leader 收到副本的 LocalOrder 并缓存
bool HotStuffCore::on_receive_local_order(const LocalOrder &local_order, const std::vector<block_t> &parents) {
    LOG_PROTO("got %s", std::string(local_order).c_str());

    /** 构建 OrderedList 并添加到 storage **/
    OrderedList olist;
    olist.cmds = local_order.ordered_hashes;
    olist.timestamps = local_order.timestamps;
    storage->add_local_order(local_order.initiator, olist);

    // envelope debug: dump the full observation row (receive order) per replica
    // chunked: short lines keep each write within one stdio buffer flush,
    // so concurrent threads cannot interleave inside a row
    if (std::getenv("FO_DBG_ROWS")) {
        const size_t n = local_order.ordered_hashes.size();
        for (size_t off = 0, part = 0; off < n; off += 15, part++) {
            std::string hs;
            for (size_t t = off; t < std::min(n, off + 15); t++)
                hs += (t == off ? "" : ",") + get_hex(local_order.ordered_hashes[t]);
            HOTSTUFF_LOG_INFO("[ROW_DBG] from=%d part=%zu/%zu %s",
                              (int)local_order.initiator, part, (n + 14) / 15,
                              hs.c_str());
        }
    }

    /** 检查是否达到 majority（FO_WAIT_ALL_ROWS=1 时等全部副本，仅测试用） **/
    size_t lo_threshold = config.nmajority;
    if (std::getenv("FO_WAIT_ALL_ROWS"))
        lo_threshold = config.nreplicas;
    if (storage->get_local_order_cache_size() >= lo_threshold) {
        return true;
    }
    HOTSTUFF_LOG_DEBUG("[[on_receive_local_order]] [fromR-%d] [thisL-%d] No majority Found", local_order.initiator, get_id());
    return false;
}

/*** end HotStuff protocol logic ***/
void HotStuffCore::on_init(uint32_t nfaulty, double fairness_parameter, double gen_param, double network_param) {
    config.nmajority = config.nreplicas - nfaulty;
    config.fairness_parameter = fairness_parameter;

    if (const char *value = std::getenv("FLASHORDER_P_KD")) {
        char *end = nullptr;
        const double parsed = std::strtod(value, &end);
        if (end != value && parsed >= 0.0)
            config.preference_disagreement_k = parsed;
    }
    
    if (gen_param > 0) {
        config.gen_param = gen_param;
        config.auto_param = false;
    }
    if (network_param > 0) {
        config.network_param = network_param;
        config.auto_param = false;
    }
    
    config.solid_tx_threshold = get_solid_tx_threshold();
    config.non_blank_tx_threshold = get_non_blank_tx_threshold();
    config.tx_edge_threshold = get_tx_edge_threshold();
    HOTSTUFF_LOG_INFO("[[on_init-P]] [R-%d] nmajority = %lu, fairness_parameter = %f, K_D = %.3f",
        get_id(), config.nmajority, config.fairness_parameter, 
        config.preference_disagreement_k);
    b0->qc = create_quorum_cert(b0->get_hash());
    b0->qc->compute();
    b0->self_qc = b0->qc->clone();
    b0->qc_ref = b0;
    hqc = std::make_pair(b0, b0->qc->clone());
}

double HotStuffCore::get_solid_tx_threshold() {
    size_t nmajority = config.nmajority;
    size_t n = config.nreplicas;
    size_t f = n - nmajority;
    return n - 2.0*f;
}

double HotStuffCore::get_non_blank_tx_threshold() {
    size_t nmajority = config.nmajority;
    size_t n = config.nreplicas;
    size_t f = n - nmajority;
    double gama = config.fairness_parameter;
    double solid = n - 2.0*f;
    double shaded = (n * (1.0-gama)) + f + 1.0;
    return solid > shaded ? shaded : solid;
}

double HotStuffCore::get_tx_edge_threshold() {
    size_t nmajority = config.nmajority;
    size_t n = config.nreplicas;
    size_t f = n - nmajority;
    double gama = config.fairness_parameter;
    return (n * (1.0-gama)) + f + 1.0;
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
