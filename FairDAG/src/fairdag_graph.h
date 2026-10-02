#ifndef _FAIRDAG_GRAPH_H
#define _FAIRDAG_GRAPH_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hotstuff {

/**
 * Tournament dependency graph used by the FairDAG-RL fairness layer.
 *
 * Nodes are transaction identifiers.  A directed edge a -> b means the fair
 * layer has enough evidence that transaction a should appear before b.  When
 * the graph becomes a complete tournament, GetOrder() condenses cycles with
 * Tarjan SCC, topologically orders the SCC DAG, and unspools each SCC with a
 * tournament Hamiltonian path insertion routine.
 */
class FairDAGGraph {
public:
    FairDAGGraph() = default;

    void AddNode(uint64_t id);
    void AddEdge(uint64_t from, uint64_t to);
    bool IsTournament() const;
    std::vector<uint64_t> GetOrder() const;
    void ClearGraph();
    std::vector<uint64_t> GetAllNode() const;
    size_t Size() const;

private:
    using AdjList = std::unordered_map<uint64_t, std::unordered_set<uint64_t>>;

    mutable std::mutex mutex_;
    std::unordered_set<uint64_t> nodes_;
    AdjList edges_;

    bool HasEdgeUnlocked(uint64_t from, uint64_t to) const;
    std::vector<std::vector<uint64_t>> TarjanSCCUnlocked() const;
    std::vector<int> TopologicalSCCOrderUnlocked(
        const std::vector<std::vector<uint64_t>> &sccs) const;
    std::vector<uint64_t> HamiltonianPathUnlocked(
        std::vector<uint64_t> nodes) const;
};

} // namespace hotstuff

#endif
