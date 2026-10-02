#ifndef _FAIRDAG_CONSENSUS_H
#define _FAIRDAG_CONSENSUS_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hotstuff/type.h"
#include "src/fairdag_graph.h"

namespace hotstuff {

using Transaction = uint256_t;

class FairDAGConsensus {
public:
    FairDAGConsensus(size_t nreplicas = 0, size_t nfaulty = 0,
                     double gamma = 1.0);

    void Configure(size_t nreplicas, size_t nfaulty, double gamma);
    void ProcessLocalOrdering(const std::vector<Transaction> &txs,
                              int proposer_id);
    void AddEdges();
    std::vector<Transaction> BuildFinalOrdering();

    std::vector<Transaction> GetLastFinalOrdering() const;
    std::vector<Transaction> DrainCommittedOrdering();
    size_t GetShadedThreshold() const;
    size_t GetSolidThreshold() const;

private:
    struct EdgeKey {
        uint64_t from;
        uint64_t to;

        bool operator==(const EdgeKey &other) const {
            return from == other.from && to == other.to;
        }
        bool operator<(const EdgeKey &other) const {
            if (from != other.from) return from < other.from;
            return to < other.to;
        }
    };

    mutable std::mutex mutex_;
    FairDAGGraph graph_;

    size_t nreplicas_ = 0;
    size_t nfaulty_ = 0;
    double gamma_ = 1.0;
    size_t shaded_threshold_ = 1;
    size_t solid_threshold_ = 1;
    uint64_t next_id_ = 0;

    std::unordered_map<Transaction, uint64_t> tx_to_id_;
    std::unordered_map<uint64_t, Transaction> id_to_tx_;
    std::unordered_set<uint64_t> all_tx_ids_;

    std::map<EdgeKey, std::set<int>> edge_proposers_;
    std::unordered_map<uint64_t, std::set<int>> txn_proposers_;
    std::unordered_set<uint64_t> solid_ids_;

    std::set<EdgeKey> possible_edges_;
    std::unordered_set<int> proposers_seen_;
    std::unordered_map<int, std::set<uint64_t>> pending_txn_ids_;
    std::unordered_set<uint64_t> all_pending_txn_ids_;

    std::vector<uint64_t> re_add_ids_;
    std::vector<Transaction> last_final_ordering_;
    std::vector<Transaction> committed_ordering_;

    uint64_t GetTransactionIdLocked(const Transaction &tx);
    size_t AggregateWeightLocked(uint64_t from, uint64_t to) const;
    void AddEdgesLocked();
    std::vector<Transaction> BuildFinalOrderingLocked();
    void ResetRoundLocked();
    void AddImplictWeightLocked();
};

} // namespace hotstuff
#endif
