#include "src/fairdag_consensus.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "hotstuff/util.h"

namespace hotstuff {

FairDAGConsensus::FairDAGConsensus(size_t nreplicas, size_t nfaulty,
                                   double gamma) {
    Configure(nreplicas, nfaulty, gamma);
}

void FairDAGConsensus::Configure(size_t nreplicas, size_t nfaulty,
                                 double gamma) {
    std::lock_guard<std::mutex> guard(mutex_);
    nreplicas_ = nreplicas;
    nfaulty_ = nfaulty;
    gamma_ = gamma;

    shaded_threshold_ = nfaulty_ + 1;
    solid_threshold_ = 1;
    ResetRoundLocked();

    HOTSTUFF_LOG_INFO("[[fairdag_config]] nreplicas=%zu nfaulty=%zu gamma=%.3f shaded_threshold=%zu solid_threshold=%zu",
                      nreplicas_, nfaulty_, gamma_, shaded_threshold_, solid_threshold_);
}

uint64_t FairDAGConsensus::GetTransactionIdLocked(const Transaction &tx) {
    auto found = tx_to_id_.find(tx);
    if (found != tx_to_id_.end())
        return found->second;

    uint64_t id = next_id_++;
    tx_to_id_[tx] = id;
    id_to_tx_[id] = tx;
    all_tx_ids_.insert(id);
    return id;
}

size_t FairDAGConsensus::AggregateWeightLocked(uint64_t from,
                                               uint64_t to) const {
    auto it = edge_proposers_.find(EdgeKey{from, to});
    return it == edge_proposers_.end() ? 0 : it->second.size();
}

void FairDAGConsensus::AddImplictWeightLocked() {
    for (int proposer = 0; proposer < static_cast<int>(nreplicas_); proposer++) {
        if (proposers_seen_.find(proposer) == proposers_seen_.end())
            continue;
        for (uint64_t id1 : pending_txn_ids_[proposer]) {
            for (uint64_t id2 : all_pending_txn_ids_) {
                if (pending_txn_ids_[proposer].find(id2) !=
                    pending_txn_ids_[proposer].end())
                    continue;
                EdgeKey edge{id1, id2};
                edge_proposers_[edge].insert(proposer);
                if (edge_proposers_[edge].size() >= shaded_threshold_)
                    possible_edges_.insert(edge);
            }
        }
    }
}

void FairDAGConsensus::ProcessLocalOrdering(const std::vector<Transaction> &txs,
                                            int proposer_id) {
    auto start_ts = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> guard(mutex_);
    if (txs.empty()) return;

    proposers_seen_.insert(proposer_id);

    std::vector<uint64_t> ids;
    ids.reserve(txs.size());
    std::unordered_set<uint64_t> seen;
    for (const auto &tx : txs) {
        uint64_t id = GetTransactionIdLocked(tx);
        if (seen.insert(id).second)
            ids.push_back(id);
    }

    for (uint64_t id : ids)
        txn_proposers_[id].insert(proposer_id);

    for (uint64_t id : ids) {
        if (txn_proposers_[id].size() >= solid_threshold_)
            solid_ids_.insert(id);
    }

    size_t new_possible_edges = 0;

    for (size_t i = 0; i < ids.size(); ++i) {
        for (size_t j = i + 1; j < ids.size(); ++j) {
            EdgeKey edge{ids[i], ids[j]};
            edge_proposers_[edge].insert(proposer_id);
            if (edge_proposers_[edge].size() >= shaded_threshold_) {
                if (possible_edges_.insert(edge).second)
                    ++new_possible_edges;
            }
        }
    }

    for (uint64_t id2 : ids) {
        for (uint64_t id1 : pending_txn_ids_[proposer_id]) {
            EdgeKey edge{id1, id2};
            edge_proposers_[edge].insert(proposer_id);
            if (edge_proposers_[edge].size() >= shaded_threshold_) {
                if (possible_edges_.insert(edge).second)
                    ++new_possible_edges;
            }
        }
    }

    for (uint64_t id : ids) {
        pending_txn_ids_[proposer_id].insert(id);
        all_pending_txn_ids_.insert(id);
    }

    AddImplictWeightLocked();
    AddEdgesLocked();

    bool ready = graph_.IsTournament();
    size_t graph_size = graph_.Size();

    if (ready)
        BuildFinalOrderingLocked();

    auto end_ts = std::chrono::steady_clock::now();
    double total_ms = std::chrono::duration_cast<std::chrono::microseconds>(
        end_ts - start_ts).count() / 1000.0;
    HOTSTUFF_LOG_INFO("[perf] fairdag_process: proposer=%d txns=%zu graph_v=%zu solid=%zu edges=%zu new_edges=%zu tournament=%d total_ms=%.3f",
                      proposer_id, ids.size(), graph_size,
                      solid_ids_.size(), possible_edges_.size(),
                      new_possible_edges, ready ? 1 : 0, total_ms);
}

void FairDAGConsensus::AddEdges() {
    std::lock_guard<std::mutex> guard(mutex_);
    AddEdgesLocked();
}

void FairDAGConsensus::AddEdgesLocked() {
    graph_.ClearGraph();

    for (uint64_t id : all_tx_ids_) {
        auto it = txn_proposers_.find(id);
        if (it != txn_proposers_.end() && it->second.size() >= shaded_threshold_)
            graph_.AddNode(id);
    }

    for (uint64_t id : re_add_ids_)
        graph_.AddNode(id);
    re_add_ids_.clear();

    std::set<std::pair<uint64_t, uint64_t>> handled;
    for (const auto &edge : possible_edges_) {
        uint64_t a = edge.from, b = edge.to;
        if (a > b) std::swap(a, b);
        if (!handled.insert({a, b}).second)
            continue;

        size_t w_ab = AggregateWeightLocked(a, b);
        size_t w_ba = AggregateWeightLocked(b, a);

        if (w_ab >= w_ba)
            graph_.AddEdge(a, b);
        else
            graph_.AddEdge(b, a);
    }
}

std::vector<Transaction> FairDAGConsensus::BuildFinalOrdering() {
    std::lock_guard<std::mutex> guard(mutex_);
    return BuildFinalOrderingLocked();
}

std::vector<Transaction> FairDAGConsensus::BuildFinalOrderingLocked() {
    auto start_ts = std::chrono::steady_clock::now();
    if (!graph_.IsTournament())
        return {};

    std::vector<uint64_t> ordered_ids = graph_.GetOrder();
    std::vector<Transaction> ordered_txs;
    ordered_txs.reserve(ordered_ids.size());

    size_t solid_count = 0, non_solid = 0;
    for (uint64_t id : ordered_ids) {
        if (solid_ids_.find(id) != solid_ids_.end()) {
            auto it = id_to_tx_.find(id);
            if (it != id_to_tx_.end())
                ordered_txs.push_back(it->second);
            ++solid_count;
        } else {
            re_add_ids_.push_back(id);
            ++non_solid;
        }
    }

    last_final_ordering_ = ordered_txs;
    committed_ordering_.insert(committed_ordering_.end(),
                               ordered_txs.begin(), ordered_txs.end());

    std::unordered_set<uint64_t> committed_ids = solid_ids_;
    for (uint64_t id : committed_ids) {
        txn_proposers_.erase(id);
        all_tx_ids_.erase(id);
        all_pending_txn_ids_.erase(id);
        for (auto &kv : pending_txn_ids_)
            kv.second.erase(id);
    }
    solid_ids_.clear();

    auto it = edge_proposers_.begin();
    while (it != edge_proposers_.end()) {
        if (committed_ids.find(it->first.from) != committed_ids.end() ||
            committed_ids.find(it->first.to) != committed_ids.end()) {
            possible_edges_.erase(it->first);
            it = edge_proposers_.erase(it);
        } else {
            ++it;
        }
    }

    auto end_ts = std::chrono::steady_clock::now();
    double total_ms = std::chrono::duration_cast<std::chrono::microseconds>(
        end_ts - start_ts).count() / 1000.0;
    HOTSTUFF_LOG_INFO("[perf] fairdag_finalize: graph_v=%zu order=%zu solid=%zu non_solid=%zu total_ms=%.3f",
                      ordered_ids.size(), ordered_txs.size(),
                      solid_count, non_solid, total_ms);

    ResetRoundLocked();
    return ordered_txs;
}

void FairDAGConsensus::ResetRoundLocked() {
    graph_.ClearGraph();
    proposers_seen_.clear();
}

std::vector<Transaction> FairDAGConsensus::GetLastFinalOrdering() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return last_final_ordering_;
}

std::vector<Transaction> FairDAGConsensus::DrainCommittedOrdering() {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Transaction> out;
    out.swap(committed_ordering_);
    return out;
}

size_t FairDAGConsensus::GetShadedThreshold() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return shaded_threshold_;
}

size_t FairDAGConsensus::GetSolidThreshold() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return solid_threshold_;
}

} // namespace hotstuff
