#ifndef _AUNCEL_AUNCEL_MSG_H
#define _AUNCEL_AUNCEL_MSG_H

#include "hotstuff/consensus.h"
#include "hotstuff/type.h"

namespace hotstuff {

struct NewView: public Serializable {
    ReplicaID replica_id;
    uint32_t view;
    quorum_cert_bt high_qc;
    std::vector<uint256_t> tx_list;
    part_cert_bt cert;
    HotStuffCore *hsc;

    NewView(): high_qc(nullptr), cert(nullptr), hsc(nullptr) {}
    NewView(ReplicaID rid, uint32_t v, quorum_cert_bt &&qc,
            std::vector<uint256_t> &&txs, part_cert_bt &&c, HotStuffCore *hsc):
        replica_id(rid), view(v), high_qc(std::move(qc)),
        tx_list(std::move(txs)), cert(std::move(c)), hsc(hsc) {}

    NewView(NewView &&other) = default;
    NewView(const NewView &other):
        replica_id(other.replica_id), view(other.view),
        high_qc(other.high_qc ? other.high_qc->clone() : nullptr),
        tx_list(other.tx_list),
        cert(other.cert ? other.cert->clone() : nullptr),
        hsc(other.hsc) {}

    void serialize(DataStream &s) const override {
        s << replica_id << view;
        if (high_qc) s << *high_qc;
        s << htole((uint32_t)tx_list.size());
        for (const auto& tx : tx_list) s << tx;
        if (cert) s << *cert;
    }

    void unserialize(DataStream &s) override {
        assert(hsc != nullptr);
        s >> replica_id >> view;
        high_qc = hsc->parse_quorum_cert(s);
        uint32_t n; s >> n; n = letoh(n);
        tx_list.resize(n);
        for (auto& tx : tx_list) s >> tx;
        cert = hsc->parse_part_cert(s);
    }
};

struct TxSync: public Serializable {
    uint32_t view;
    ReplicaID leader_id;            /* CSC Fix1: actual leader for this view */
    uint256_t leader_block_hash;    /* CSC Fix2: block hash for Sort-Vote signing */
    quorum_cert_bt high_qc;
    std::vector<uint256_t> cur_txs;
    HotStuffCore *hsc;

    TxSync(): leader_id(0), high_qc(nullptr), hsc(nullptr) {}
    TxSync(uint32_t v, ReplicaID lid, const uint256_t &lbh,
           quorum_cert_bt &&qc, std::vector<uint256_t> &&txs, HotStuffCore *hsc):
        view(v), leader_id(lid), leader_block_hash(lbh),
        high_qc(std::move(qc)), cur_txs(std::move(txs)), hsc(hsc) {}

    TxSync(TxSync &&other) = default;
    TxSync(const TxSync &other):
        view(other.view),
        leader_id(other.leader_id),
        leader_block_hash(other.leader_block_hash),
        high_qc(other.high_qc ? other.high_qc->clone() : nullptr),
        cur_txs(other.cur_txs), hsc(other.hsc) {}

    void serialize(DataStream &s) const override {
        s << view << leader_id << leader_block_hash;
        if (high_qc) s << *high_qc;
        s << htole((uint32_t)cur_txs.size());
        for (const auto &tx : cur_txs) s << tx;
    }

    void unserialize(DataStream &s) override {
        assert(hsc != nullptr);
        s >> view >> leader_id >> leader_block_hash;
        high_qc = hsc->parse_quorum_cert(s);
        uint32_t n; s >> n; n = letoh(n);
        cur_txs.resize(n);
        for (auto& tx : cur_txs) s >> tx;
    }
};

struct SortVote: public Serializable {
    ReplicaID replica_id;
    uint32_t view;
    std::vector<uint256_t> local_ordering;
    std::vector<uint32_t> ordering_ids;
    std::vector<uint256_t> missing_txs;
    part_cert_bt cert;
    HotStuffCore *hsc;

    SortVote(): cert(nullptr), hsc(nullptr) {}
    SortVote(ReplicaID rid, uint32_t v,
             std::vector<uint256_t> &&ordering,
             std::vector<uint32_t> &&ids,
             std::vector<uint256_t> &&missing,
             part_cert_bt &&c, HotStuffCore *hsc):
        replica_id(rid), view(v),
        local_ordering(std::move(ordering)),
        ordering_ids(std::move(ids)),
        missing_txs(std::move(missing)),
        cert(std::move(c)), hsc(hsc) {}

    SortVote(SortVote &&other) = default;
    SortVote(const SortVote &other):
        replica_id(other.replica_id), view(other.view),
        local_ordering(other.local_ordering),
        ordering_ids(other.ordering_ids),
        missing_txs(other.missing_txs),
        cert(other.cert ? other.cert->clone() : nullptr),
        hsc(other.hsc) {}

    void serialize(DataStream &s) const override {
        s << replica_id << view;
        s << htole((uint32_t)ordering_ids.size());
        for (const auto &id : ordering_ids) s << htole(id);
        s << htole((uint32_t)local_ordering.size());
        for (const auto &tx : local_ordering) s << tx;
        s << htole((uint32_t)missing_txs.size());
        for (const auto &tx : missing_txs) s << tx;
        if (cert) s << *cert;
    }

    void unserialize(DataStream &s) override {
        assert(hsc != nullptr);
        s >> replica_id >> view;
        uint32_t n; s >> n; n = letoh(n);
        ordering_ids.resize(n);
        for (auto &id : ordering_ids) { s >> id; id = letoh(id); }
        s >> n; n = letoh(n);
        local_ordering.resize(n);
        for (auto& tx : local_ordering) s >> tx;
        s >> n; n = letoh(n);
        missing_txs.resize(n);
        for (auto& tx : missing_txs) s >> tx;
        cert = hsc->parse_part_cert(s);
    }
};

}

#endif
