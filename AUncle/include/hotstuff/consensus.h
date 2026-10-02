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

#ifndef _HOTSTUFF_CONSENSUS_H
#define _HOTSTUFF_CONSENSUS_H

#include <cassert>
#include <mutex>
#include <set>
#include <unordered_map>

#include "hotstuff/promise.hpp"
#include "hotstuff/type.h"
#include "hotstuff/entity.h"
#include "hotstuff/crypto.h"

namespace hotstuff {

static inline uint256_t compute_vote_hash(const uint256_t &block_hash,
                                           const std::vector<uint256_t> &local_ordering) {
    DataStream s;
    s << block_hash;
    for (const auto &tx : local_ordering) s << tx;
    return s.get_hash();
}

struct Proposal;
struct Vote;
struct Finality;

/** Abstraction for HotStuff protocol state machine (without network implementation). */
class HotStuffCore {
    block_t b0;                                  /** the genesis block */
    /* === state variables === */
    /** block containing the QC for the highest block having one */
    std::pair<block_t, quorum_cert_bt> hqc;   /**< highest QC */
    block_t b_lock;                            /**< locked block */
    block_t b_exec;                            /**< last executed block */
    uint32_t vheight;          /**< height of the block last voted for */
    /* === auxilliary variables === */
    privkey_bt priv_key;            /**< private key for signing votes */
    std::set<block_t> tails;   /**< set of tail blocks */
    ReplicaConfig config;                   /**< replica configuration */
    /* === async event queues === */
    std::unordered_map<block_t, promise_t> qc_waiting;
    promise_t propose_waiting;
    promise_t receive_proposal_waiting;
    promise_t hqc_update_waiting;
    /* == feature switches == */
    /** always vote negatively, useful for some PaceMakers */
    bool vote_disabled;

    block_t get_delivered_blk(const uint256_t &blk_hash);
    void sanity_check_delivered(const block_t &blk);
    void update(const block_t &nblk);
    void update_hqc(const block_t &_hqc, const quorum_cert_bt &qc);
    void on_hqc_update();
    void on_qc_finish(const block_t &blk);
    void on_propose_(const Proposal &prop);
    void on_receive_proposal_(const Proposal &prop);

    protected:
    ReplicaID id;                  /**< identity of the replica itself */

    public:
    BoxObj<EntityStorage> storage;

    std::deque<uint256_t> receive_order_log;
    std::unordered_set<uint256_t> receive_order_set;
    std::unordered_set<uint256_t> committed_cmds;
    /** Serializes access to receive_order_log / receive_order_set /
     *  committed_cmds. record_received_cmd() runs on client-network threads
     *  (via exec_command) while exec_blk()/on_tx_sync() run on replica-network
     *  and main threads. */
    mutable std::mutex order_log_mutex;
    double weight_k = 0.5;

    std::unordered_map<uint32_t, std::vector<std::vector<uint256_t>>> pending_sort_votes;
    std::unordered_map<uint32_t, std::unordered_map<ReplicaID, part_cert_bt>> pending_sort_certs;
    std::unordered_map<uint32_t, uint256_t> view_to_block;
    std::unordered_map<uint256_t, size_t> missing_vote_count;
    std::unordered_set<uint256_t> censorship_alerted;
    bool enable_censorship_detection = false;

    block_t pending_proposal;
    uint32_t pending_view = 0;

protected:
    HotStuffCore(ReplicaID id, privkey_bt &&priv_key);
    virtual ~HotStuffCore() {
        b0->qc_ref = nullptr;
    }

public:
    /* Inputs of the state machine triggered by external events, should called
     * by the class user, with proper invariants. */

    /** Call to initialize the protocol, should be called once before all other
     * functions. */
    void on_init(uint32_t nfaulty);

    /* TODO: better name for "delivery" ? */
    /** Call to inform the state machine that a block is ready to be handled.
     * A block is only delivered if itself is fetched, the block for the
     * contained qc is fetched and all parents are delivered. The user should
     * always ensure this invariant. The invalid blocks will be dropped by this
     * function.
     * @return true if valid */
    bool on_deliver_blk(const block_t &blk);

    /** Call upon the delivery of a proposal message.
     * The block mentioned in the message should be already delivered. */
    void on_receive_proposal(const Proposal &prop);

    /** Call upon the delivery of a vote message.
     * The block mentioned in the message should be already delivered. */
    void on_receive_vote(const Vote &vote);

    /** Call to submit new commands to be decided (executed). "Parents" must
     * contain at least one block, and the first block is the actual parent,
     * while the others are uncles/aunts */
    block_t on_propose(const std::vector<uint256_t> &cmds,
                    const std::vector<block_t> &parents,
                    bytearray_t &&extra = bytearray_t());

    void record_received_cmd(const uint256_t &cmd) {
        std::lock_guard<std::mutex> lk(order_log_mutex);
        if (receive_order_set.insert(cmd).second) {
            receive_order_log.push_back(cmd);
        }
    }

    std::vector<uint256_t> get_local_ordering(const std::unordered_set<uint256_t> &tx_subset) const {
        std::lock_guard<std::mutex> lk(order_log_mutex);
        std::vector<uint256_t> result;
        for (const auto& cmd : receive_order_log) {
            if (committed_cmds.count(cmd)) continue;
            if (tx_subset.count(cmd)) result.push_back(cmd);
        }
        return result;
    }

    void compact_receive_order_log(size_t max_size = 5000) {
        std::lock_guard<std::mutex> lk(order_log_mutex);
        if (receive_order_log.size() <= max_size && committed_cmds.empty())
            return;
        std::deque<uint256_t> new_log;
        for (const auto& cmd : receive_order_log) {
            if (committed_cmds.count(cmd)) {
                committed_cmds.erase(cmd);
            } else {
                new_log.push_back(cmd);
            }
        }
        receive_order_log.swap(new_log);
    }

    void set_weight_k(double k) { weight_k = (k > 0.0 && k < 1.0) ? k : 0.5; }

    void on_tx_sync(uint32_t view, const std::vector<uint256_t> &cur_txs,
                    ReplicaID leader_id, const uint256_t &leader_block_hash);
    void on_sort_vote(ReplicaID replica, uint32_t view,
                      const std::vector<uint256_t> &local_ordering,
                      const std::vector<uint32_t> &ordering_ids,
                      const std::vector<uint256_t> &missing_txs,
                      part_cert_bt &&cert);

    void finalize_pending_proposal();

    quorum_cert_bt clone_hqc_qc() { return hqc.second ? hqc.second->clone() : nullptr; }
    part_cert_bt sign_blank(const uint256_t &obj) {
        return create_part_cert(*priv_key, obj);
    }



    /* Functions required to construct concrete instances for abstract classes.
     * */

    /* Outputs of the state machine triggering external events.  The virtual
     * functions should be implemented by the user to specify the behavior upon
     * the events. */
    protected:
    /** Called by HotStuffCore upon the decision being made for cmd. */
    virtual void do_decide(Finality &&fin) = 0;
    virtual void do_consensus(const block_t &blk) = 0;
    /** Called by HotStuffCore upon broadcasting a new proposal.
     * The user should send the proposal message to all replicas except for
     * itself. */
    virtual void do_broadcast_proposal(const Proposal &prop) = 0;
    /** Called upon sending out a new vote to the next proposer.  The user
     * should send the vote message to a *good* proposer to have good liveness,
     * while safety is always guaranteed by HotStuffCore. */
    virtual void do_vote(ReplicaID last_proposer, const Vote &vote) = 0;

    virtual void do_send_tx_sync(uint32_t view, const std::vector<uint256_t> &cur_txs,
                                 ReplicaID leader_id, const uint256_t &leader_block_hash) {}
    virtual void do_send_sort_vote(ReplicaID leader,
                                   const std::vector<uint256_t> &local_ordering,
                                   const std::vector<uint32_t> &ordering_ids,
                                   const std::vector<uint256_t> &missing_txs,
                                   uint32_t view,
                                   const uint256_t &block_hash) {}
    /** CSC VVC: Called on view change to send NewView (pending txs + HighQC) to the new leader. */
    virtual void do_send_new_view(ReplicaID new_leader, uint32_t view) {}

    /* The user plugs in the detailed instances for those
     * polymorphic data types. */
    public:
    virtual void on_view_change() {}
    /** Create a partial certificate that proves the vote for a block. */
    virtual part_cert_bt create_part_cert(const PrivKey &priv_key, const uint256_t &blk_hash) = 0;
    /** Create a partial certificate from its seralized form. */
    virtual part_cert_bt parse_part_cert(DataStream &s) = 0;
    /** Create a quorum certificate that proves 2f+1 votes for a block. */
    virtual quorum_cert_bt create_quorum_cert(const uint256_t &blk_hash) = 0;
    /** Create a quorum certificate from its serialized form. */
    virtual quorum_cert_bt parse_quorum_cert(DataStream &s) = 0;
    /** Create a command object from its serialized form. */
    //virtual command_t parse_cmd(DataStream &s) = 0;

    public:
    /** Add a replica to the current configuration. This should only be called
     * before running HotStuffCore protocol. */
    void add_replica(ReplicaID rid, const PeerId &peer_id, pubkey_bt &&pub_key);
    /** Try to prune blocks lower than last committed height - staleness. */
    void prune(uint32_t staleness);

    /* PaceMaker can use these functions to monitor the core protocol state
     * transition */
    /** Get a promise resolved when the block gets a QC. */
    promise_t async_qc_finish(const block_t &blk);
    /** Get a promise resolved when a new block is proposed. */
    promise_t async_wait_proposal();
    /** Get a promise resolved when a new proposal is received. */
    promise_t async_wait_receive_proposal();
    /** Get a promise resolved when hqc is updated. */
    promise_t async_hqc_update();

    /* Other useful functions */
    const block_t &get_genesis() const { return b0; }
    const block_t &get_hqc() { return hqc.first; }
    const ReplicaConfig &get_config() const { return config; }
    ReplicaID get_id() const { return id; }
    const std::set<block_t> get_tails() const { return tails; }
    operator std::string () const;
    void set_vote_disabled(bool f) { vote_disabled = f; }
};

/** Abstraction for proposal messages. */
struct Proposal: public Serializable {
    ReplicaID proposer;
    block_t blk;
    std::vector<std::vector<uint256_t>> ordering_map;
    std::vector<ReplicaID> vote_replica_ids;
    std::vector<std::vector<uint256_t>> vote_orderings;
    std::vector<part_cert_bt> vote_certs;
    HotStuffCore *hsc;

    Proposal(): blk(nullptr), hsc(nullptr) {}
    Proposal(ReplicaID proposer,
            const block_t &blk,
            HotStuffCore *hsc):
        proposer(proposer),
        blk(blk), hsc(hsc) {}

    Proposal(const Proposal &other):
        proposer(other.proposer),
        blk(other.blk),
        ordering_map(other.ordering_map),
        vote_replica_ids(other.vote_replica_ids),
        vote_orderings(other.vote_orderings),
        hsc(other.hsc) {
        for (const auto &c : other.vote_certs)
            vote_certs.push_back(c ? c->clone() : nullptr);
    }

    Proposal(Proposal &&other) = default;

    void serialize(DataStream &s) const override {
        s << proposer
          << *blk;
        s << htole((uint32_t)ordering_map.size());
        for (const auto &ordering : ordering_map)
        {
            s << htole((uint32_t)ordering.size());
            for (const auto &h : ordering)
                s << h;
        }
        const auto &oqc = blk->get_order_qc();
        s << htole((uint32_t)(oqc ? 1 : 0));
        if (oqc) s << *oqc;
        s << htole((uint32_t)vote_replica_ids.size());
        for (size_t i = 0; i < vote_replica_ids.size(); i++)
        {
            s << vote_replica_ids[i];
            s << htole((uint32_t)vote_orderings[i].size());
            for (const auto &h : vote_orderings[i]) s << h;
            if (vote_certs[i]) s << *vote_certs[i];
        }
    }

    void unserialize(DataStream &s) override {
        assert(hsc != nullptr);
        s >> proposer;
        Block _blk;
        _blk.unserialize(s, hsc);
        blk = hsc->storage->add_blk(std::move(_blk), hsc->get_config());
        uint32_t n;
        s >> n;
        n = letoh(n);
        ordering_map.resize(n);
        for (auto &ordering : ordering_map)
        {
            uint32_t m;
            s >> m;
            m = letoh(m);
            ordering.resize(m);
            for (auto &h : ordering)
                s >> h;
        }
        uint32_t has_oqc;
        s >> has_oqc; has_oqc = letoh(has_oqc);
        if (has_oqc)
        {
            auto oqc = hsc->parse_quorum_cert(s);
            blk->set_order_qc(std::move(oqc));
        }
        uint32_t nvotes;
        s >> nvotes; nvotes = letoh(nvotes);
        vote_replica_ids.resize(nvotes);
        vote_orderings.resize(nvotes);
        vote_certs.resize(nvotes);
        for (size_t i = 0; i < nvotes; i++)
        {
            s >> vote_replica_ids[i];
            uint32_t m;
            s >> m; m = letoh(m);
            vote_orderings[i].resize(m);
            for (auto &h : vote_orderings[i]) s >> h;
            vote_certs[i] = hsc->parse_part_cert(s);
        }
    }

    operator std::string () const {
        DataStream s;
        s << "<proposal "
          << "rid=" << std::to_string(proposer) << " "
          << "blk=" << get_hex10(blk->get_hash()) << ">";
        return s;
    }
};

/** Abstraction for vote messages. */
struct Vote: public Serializable {
    ReplicaID voter;
    /** block being voted */
    uint256_t blk_hash;
    /** proof of validity for the vote */
    part_cert_bt cert;
    
    /** handle of the core object to allow polymorphism */
    HotStuffCore *hsc;

    Vote(): cert(nullptr), hsc(nullptr) {}
    Vote(ReplicaID voter,
        const uint256_t &blk_hash,
        part_cert_bt &&cert,
        HotStuffCore *hsc):
        voter(voter),
        blk_hash(blk_hash),
        cert(std::move(cert)), hsc(hsc) {}

    Vote(const Vote &other):
        voter(other.voter),
        blk_hash(other.blk_hash),
        cert(other.cert ? other.cert->clone() : nullptr),
        hsc(other.hsc) {}

    Vote(Vote &&other) = default;
    
    void serialize(DataStream &s) const override {
        s << voter << blk_hash << *cert;
    }

    void unserialize(DataStream &s) override {
        assert(hsc != nullptr);
        s >> voter >> blk_hash;
        cert = hsc->parse_part_cert(s);
    }

    bool verify() const {
        assert(hsc != nullptr);
        return cert->verify(hsc->get_config().get_pubkey(voter)) &&
                cert->get_obj_hash() == blk_hash;
    }

    promise_t verify(VeriPool &vpool) const {
        assert(hsc != nullptr);
        return cert->verify(hsc->get_config().get_pubkey(voter), vpool).then([this](bool result) {
            return result && cert->get_obj_hash() == blk_hash;
        });
    }

    operator std::string () const {
        DataStream s;
        s << "<vote "
          << "rid=" << std::to_string(voter) << " "
          << "blk=" << get_hex10(blk_hash) << ">";
        return s;
    }
};

struct Finality: public Serializable {
    ReplicaID rid;
    int8_t decision;
    uint32_t cmd_idx;
    uint32_t cmd_height;
    uint256_t cmd_hash;
    uint256_t blk_hash;
    
    public:
    Finality() = default;
    Finality(ReplicaID rid,
            int8_t decision,
            uint32_t cmd_idx,
            uint32_t cmd_height,
            uint256_t cmd_hash,
            uint256_t blk_hash):
        rid(rid), decision(decision),
        cmd_idx(cmd_idx), cmd_height(cmd_height),
        cmd_hash(cmd_hash), blk_hash(blk_hash) {}

    void serialize(DataStream &s) const override {
        s << rid << decision
          << cmd_idx << cmd_height
          << cmd_hash;
        if (decision == 1) s << blk_hash;
    }

    void unserialize(DataStream &s) override {
        s >> rid >> decision
          >> cmd_idx >> cmd_height
          >> cmd_hash;
        if (decision == 1) s >> blk_hash;
    }

    operator std::string () const {
        DataStream s;
        s << "<fin "
          << "decision=" << std::to_string(decision) << " "
          << "cmd_idx=" << std::to_string(cmd_idx) << " "
          << "cmd_height=" << std::to_string(cmd_height) << " "
          << "cmd=" << get_hex10(cmd_hash) << " "
          << "blk=" << get_hex10(blk_hash) << ">";
        return s;
    }
};

}

#endif
