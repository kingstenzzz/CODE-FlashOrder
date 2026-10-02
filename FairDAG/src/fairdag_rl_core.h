// FairDAG-RL upstream ordering core — offline semantics, single translation unit.
//
// Mirrors github_code/fairdag-rl-reference/python/fairdag_rl_upstream_port.py
// (`upstream_order`, paper_thresholds=False, i.e. ARTIFACT semantics of
// apache/incubator-resilientdb @ fairdag branch, commit 4ba9410):
//
//   edge admission : pair (a,b) becomes a possible edge iff
//                    max(P[a][b], P[b][a]) >= f+1   (shaded threshold)
//   orientation    : majority of the pair, tie -> forward
//                    (a->b when P[a][b] >= P[b][a])
//   condensation   : Tarjan SCC over nodes in ascending id with ascending
//                    adjacency; SCC members emitted ascending; SCC id =
//                    discovery (finish) order
//   batch order    : Kahn FIFO over the condensation, seeded with all
//                    zero-indegree SCC ids in ascending order; a child is
//                    enqueued when its (duplicate-kept) indegree drops to 0
//   intra-batch    : ascending node id
//
// The solid threshold (upstream 2f+1) is a per-transaction proposer-count
// predicate of the ONLINE layer; it has no effect on the offline ordering
// function and is therefore not represented here (same as the Python port).
//
// Differential-test contract: for identical P, f this function must return a
// bit-identical permutation to the Python oracle
// (fairdag_rl_upstream_port.upstream_order(P, f, paper_thresholds=False)).
//
// No libhotstuff / salticidae dependencies: includable from the standalone
// differential runner AND from the deployment proposal path.
#ifndef FAIRDAG_RL_CORE_H
#define FAIRDAG_RL_CORE_H

#include <algorithm>
#include <cstddef>
#include <deque>
#include <set>
#include <vector>

namespace fairdag_rl_core {

// P is row-major m x m: P[a*m + b] = number of replicas (proposers) whose
// local ordering puts a before b. Weights are non-negative; P[a][a] is
// ignored. f is the fault bound; the admission threshold is f+1.
inline std::vector<int> upstream_order(const std::vector<long long> &P,
                                       std::size_t m, int f) {
    const int mi = static_cast<int>(m);
    std::vector<int> order;
    if (mi == 0) return order;
    order.reserve(mi);

    // ---- edge admission + orientation (AddEdges semantics) ----
    std::vector<std::vector<int>> sadj(mi);  // sorted adjacency (asc)
    {
        std::set<std::pair<int, int>> edges;  // (from, to), unique
        const long long thr = static_cast<long long>(f) + 1;
        for (int a = 0; a < mi; ++a) {
            for (int b = a + 1; b < mi; ++b) {
                const long long w_ab = P[static_cast<std::size_t>(a) * m +
                                         static_cast<std::size_t>(b)];
                const long long w_ba = P[static_cast<std::size_t>(b) * m +
                                         static_cast<std::size_t>(a)];
                if (w_ab >= thr || w_ba >= thr) {
                    if (w_ab >= w_ba) edges.insert({a, b});
                    else              edges.insert({b, a});
                }
            }
        }
        for (const auto &e : edges) sadj[e.first].push_back(e.second);
        // std::set iteration is ascending, so sadj[u] is already ascending.
    }

    // ---- Tarjan SCC (iterative; roots ascending; adjacency ascending) ----
    std::vector<int> index(mi, 0), low(mi, 0), belong(mi, -1);
    std::vector<char> on_stack(mi, 0);
    std::vector<int> stk;
    std::vector<std::vector<int>> sccs;
    int counter = 0;
    struct Frame { int u; std::size_t pos; };
    for (int root = 0; root < mi; ++root) {
        if (index[root] != 0) continue;
        index[root] = low[root] = ++counter;
        stk.push_back(root); on_stack[root] = 1;
        std::vector<Frame> work;
        work.push_back({root, 0});
        while (!work.empty()) {
            Frame &fr = work.back();
            const int u = fr.u;
            bool advanced = false;
            while (fr.pos < sadj[u].size()) {
                const int v = sadj[u][fr.pos];
                ++fr.pos;
                if (index[v] == 0) {
                    index[v] = low[v] = ++counter;
                    stk.push_back(v); on_stack[v] = 1;
                    work.push_back({v, 0});
                    advanced = true;
                    break;
                } else if (on_stack[v]) {
                    if (index[v] < low[u]) low[u] = index[v];
                }
            }
            if (advanced) continue;
            // finish u
            work.pop_back();
            if (!work.empty()) {
                const int parent = work.back().u;
                if (low[u] < low[parent]) low[parent] = low[u];
            }
            if (index[u] == low[u]) {
                std::vector<int> members;
                for (;;) {
                    const int v = stk.back();
                    stk.pop_back(); on_stack[v] = 0;
                    members.push_back(v);
                    if (v == u) break;
                }
                std::sort(members.begin(), members.end());
                const int id = static_cast<int>(sccs.size());
                for (const int v : members) belong[v] = id;
                sccs.push_back(std::move(members));
            }
        }
    }

    // ---- condensation edges (duplicate-kept, u ascending / adjacency asc) ----
    const int k = static_cast<int>(sccs.size());
    std::vector<std::vector<int>> og(k);
    std::vector<int> indeg(k, 0);
    for (int u = 0; u < mi; ++u) {
        const int bu = belong[u];
        for (const int v : sadj[u]) {
            const int bv = belong[v];
            if (bv != bu) {
                og[bu].push_back(bv);
                ++indeg[bv];
            }
        }
    }

    // ---- Kahn FIFO, seeded ascending; emit SCC members ascending ----
    std::deque<int> q;
    for (int i = 0; i < k; ++i)
        if (indeg[i] == 0) q.push_back(i);
    std::vector<char> emitted(mi, 0);
    while (!q.empty()) {
        const int u = q.front();
        q.pop_front();
        for (const int v : sccs[u]) {
            order.push_back(v);
            emitted[v] = 1;
        }
        for (const int bv : og[u]) {
            if (--indeg[bv] == 0) q.push_back(bv);
        }
    }
    // leftover (unreached nodes; mirrors the oracle's safety net)
    for (int v = 0; v < mi; ++v)
        if (!emitted[v]) order.push_back(v);
    return order;
}

}  // namespace fairdag_rl_core

#endif  // FAIRDAG_RL_CORE_H
