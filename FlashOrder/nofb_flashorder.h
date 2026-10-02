#ifndef _FLASHORDER_NOFB_H
#define _FLASHORDER_NOFB_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <vector>

#include "hotstuff/entity.h"
#include "hotstuff/type.h"
#include "hotstuff/util.h"

namespace FlashOrderNoFB {

/**
 * FlashOrder (HyperOrder-X) - High Performance Edition
 *
 * 1. Uses a 1D flat vector strictly sized to n_txns * n_txns for L1/L2 cache locality.
 * 2. Bypasses redundant sorting if timestamps are already sorted.
 * 3. Exact threshold logic without loss of precision.
 */
template <int max_number_cmds> class HyperGraph {
private:
  std::vector<uint16_t> pref;
  int canonical_pos[max_number_cmds];

  struct HyperEdge {
    std::vector<int> members;
    std::vector<int> internal_order;
  };
  std::vector<HyperEdge> hyperedges;

  std::vector<std::vector<int>> he_adj;
  std::vector<int> he_indeg;

  std::unordered_map<salticidae::uint256_t, int> cmd_to_id;
  std::vector<salticidae::uint256_t> id_to_cmd;
  int n_txns;

  int threshold;
  double gamma, k_d, margin;
  int n_nodes, n_f;

  struct PhaseProfile {
    double preferences_ms;
    double canonical_ms;
    double clusters_ms;
    double hypergraph_ms;
    double topo_sort_ms;
    double total_ms;
    int delta;
    int n_hyperedges;
    int n_sccs;
    int n_multi_sccs;
    int max_scc_size;
    int stalled_clusters = 0;
  };
  PhaseProfile last_profile;

    void compute_preferences(std::vector<hotstuff::OrderedList> &orderedlists) {
      int n_replicas = (int)orderedlists.size();
      HOTSTUFF_LOG_INFO("[[HyperGraph]] n_replicas = %d", n_replicas);
      
      for (int r = 0; r < n_replicas; r++) {
        const auto &ts = orderedlists[r].timestamps;
        if (ts.size() >= 2 && !std::is_sorted(ts.begin(), ts.end()))
          orderedlists[r].sort_cmds();
      }

      cmd_to_id.clear();
      id_to_cmd.clear();
      n_txns = 0;
      for (int r = 0; r < n_replicas; r++) {
        for (const auto &cmd : orderedlists[r].cmds) {
          if (cmd_to_id.find(cmd) == cmd_to_id.end()) {
            if (n_txns >= max_number_cmds)
              break;
            cmd_to_id[cmd] = n_txns;
            id_to_cmd.push_back(cmd);
            n_txns++;
          }
        }
      }

      HOTSTUFF_LOG_INFO("[[HyperGraph]] n_txns = %d", n_txns);
      if (n_txns == 0)
        return;

      std::vector<std::vector<int>> replica_mapped_ids(n_replicas);
      for (int r = 0; r < n_replicas; r++) {
        int n_cmds = (int)orderedlists[r].cmds.size();
        replica_mapped_ids[r].reserve(n_cmds);
        for (int i = 0; i < n_cmds; i++) {
          auto it = cmd_to_id.find(orderedlists[r].cmds[i]);
          if (it != cmd_to_id.end())
            replica_mapped_ids[r].push_back(it->second);
        }
      }

      pref.assign(n_txns * n_txns, 0);

      for (int r = 0; r < n_replicas; r++) {
        const auto &mapped_ids = replica_mapped_ids[r];
        int n_cmds = (int)mapped_ids.size();
        for (int i = 0; i < n_cmds; i++) {
          int id_i = mapped_ids[i];
          int row_id = id_i * n_txns;
          for (int j = i + 1; j < n_cmds; j++) {
            int id_j = mapped_ids[j];
            pref[row_id + id_j]++;
          }
        }
      }
      HOTSTUFF_LOG_INFO("[[HyperGraph]] compute_preferences DONE");
    }

    void compute_canonical_positions() {
      std::memset(canonical_pos, 0, sizeof(canonical_pos));
      for (int other = 0; other < n_txns; other++) {
        int row_offset = other * n_txns;
        for (int tx = 0; tx < n_txns; tx++) {
          if (other != tx && pref[row_offset + tx] >= threshold) {
            canonical_pos[tx]++;
          }
        }
      }
      HOTSTUFF_LOG_INFO("[[HyperGraph]] compute_canonical_positions DONE");
    }

    double compute_disagreement() const {
      long double numerator = 0.0L;
      long double denominator = 0.0L;
      for (int i = 0; i < n_txns; ++i) {
        for (int j = i + 1; j < n_txns; ++j) {
          const long double forward = pref[i * n_txns + j];
          const long double reverse = pref[j * n_txns + i];
          numerator += 2.0L * std::min(forward, reverse);
          denominator += forward + reverse;
        }
      }
      return denominator == 0.0L ? 0.0 : static_cast<double>(numerator / denominator);
    }

    int compute_delta() {
      const double disagreement = compute_disagreement();
      const int proposed = 1 + static_cast<int>(std::ceil(k_d * disagreement));
      const int upper = std::max(1, n_txns - 1);
      const int delta = std::max(1, std::min(upper, proposed));
      HOTSTUFF_LOG_INFO("[[HyperGraph-P]] D_t=%.6f K_D=%.3f delta=%d", disagreement, k_d, delta);
      return delta;
    }

    void detect_clusters_refined() {
      hyperedges.clear();
      if (n_txns == 0) {
        last_profile.n_hyperedges = 0;
        return;
      }
      std::vector<int> sid(n_txns);
      std::iota(sid.begin(), sid.end(), 0);
      std::sort(sid.begin(), sid.end(), [this](int a, int b) {
        return canonical_pos[a] < canonical_pos[b];
      });

      int delta = compute_delta();
      last_profile.delta = delta;
      HOTSTUFF_LOG_INFO("[[HyperGraph-P]] canonical clustering delta=%d", delta);

      HyperEdge curr;
      curr.members.push_back(sid[0]);
      for (int i = 1; i < n_txns; i++) {
        if (canonical_pos[sid[i]] - canonical_pos[sid[i - 1]] < delta) {
          curr.members.push_back(sid[i]);
        } else {
          hyperedges.push_back(curr);
          curr = HyperEdge();
          curr.members.push_back(sid[i]);
        }
      }
      hyperedges.push_back(curr);
      last_profile.n_hyperedges = (int)hyperedges.size();
      HOTSTUFF_LOG_INFO("[[HyperGraph]] detect_clusters_refined DONE, clusters=%lu", hyperedges.size());
    }

    void build_hypergraph() {
      int H = (int)hyperedges.size();
      if (H == 0)
        return;
      he_adj.assign(H, std::vector<int>());
      he_indeg.assign(H, 0);

      for (auto &he : hyperedges) {
        if (he.members.size() <= 1) {
          if (he.members.size() == 1)
            he.internal_order = he.members;
          continue;
        }
        std::vector<std::pair<long long, int>> scores;
        for (int m : he.members) {
          long long net = 0;
          int r_m = m * n_txns;
          for (int o : he.members) {
            if (m != o)
              net += (long long)pref[r_m + o] - (long long)pref[o * n_txns + m];
          }
          scores.push_back({net, m});
        }
        std::sort(scores.begin(), scores.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });
        he.internal_order.clear();
        for (auto &p : scores)
          he.internal_order.push_back(p.second);
      }

      if (H == 1)
        return;

      for (int i = 0; i < H; i++) {
        for (int j = i + 1; j < H; j++) {
          long long s_ij = 0, s_ji = 0;
          for (int a : hyperedges[i].members) {
            int off_a = a * n_txns;
            for (int b : hyperedges[j].members) {
              s_ij += pref[off_a + b];
              s_ji += pref[b * n_txns + a];
            }
          }
          if ((double)s_ij > margin * (double)s_ji) {
            he_adj[i].push_back(j);
            he_indeg[j]++;
          } else if ((double)s_ji > margin * (double)s_ij) {
            he_adj[j].push_back(i);
            he_indeg[i]++;
          }
        }
      }
      HOTSTUFF_LOG_INFO("[[HyperGraph]] build_hypergraph DONE");
    }

    void tarjan_scc(int u, std::vector<int> &scc_id,
                    std::vector<int> &index, std::vector<int> &lowlink,
                    std::vector<bool> &on_stack, std::vector<int> &stack,
                    int &timer, int &scc_count) {
      index[u] = lowlink[u] = timer++;
      stack.push_back(u);
      on_stack[u] = true;

      for (int v : he_adj[u]) {
        if (index[v] == -1) {
          tarjan_scc(v, scc_id, index, lowlink, on_stack, stack, timer, scc_count);
          lowlink[u] = std::min(lowlink[u], lowlink[v]);
        } else if (on_stack[v]) {
          lowlink[u] = std::min(lowlink[u], index[v]);
        }
      }

      if (lowlink[u] == index[u]) {
        while (true) {
          int w = stack.back();
          stack.pop_back();
          on_stack[w] = false;
          scc_id[w] = scc_count;
          if (w == u) break;
        }
        scc_count++;
      }
    }

    void build_scc_condensation(const std::vector<int> &scc_id,
                                int scc_count,
                                std::vector<std::vector<int>> &scc_adj,
                                std::vector<int> &scc_indeg) {
      scc_adj.assign(scc_count, std::vector<int>());
      scc_indeg.assign(scc_count, 0);
      // Deduplicate (source_scc, target_scc) pairs BEFORE counting indegree.
      // Hyperedges inside the same SCC can emit duplicate condensation edges;
      // counting them in scc_indeg while deduping scc_adj stalls Kahn's
      // algorithm and silently drops every transaction after the stall point.
      std::vector<std::vector<int>> pairs(scc_count);
      for (int u = 0; u < (int)hyperedges.size(); u++) {
        for (int v : he_adj[u]) {
          if (scc_id[u] != scc_id[v]) {
            pairs[scc_id[u]].push_back(scc_id[v]);
          }
        }
      }
      for (int i = 0; i < scc_count; i++) {
        std::sort(pairs[i].begin(), pairs[i].end());
        pairs[i].erase(std::unique(pairs[i].begin(), pairs[i].end()),
                       pairs[i].end());
        scc_adj[i] = pairs[i];
        for (int v : pairs[i]) scc_indeg[v]++;
      }
    }

    std::vector<int> order_within_scc(const std::vector<int> &members) {
      if (members.size() <= 1)
        return members;

      std::vector<std::pair<long long, int>> scores;
      for (int m : members) {
        long long net = 0;
        int r_m = m * n_txns;
        for (int o : members) {
          if (m != o)
            net += (long long)pref[r_m + o] - (long long)pref[o * n_txns + m];
        }
        scores.push_back({net, m});
      }
      std::stable_sort(scores.begin(), scores.end(),
                       [](const auto &a, const auto &b) {
                         if (a.first != b.first) return a.first > b.first;
                         return a.second < b.second;
                       });
      std::vector<int> result;
      for (auto &p : scores)
        result.push_back(p.second);
      return result;
    }

    std::vector<int> topological_sort() {
      // NO-FALLBACK variant: Tarjan kept ONLY for cycle detection (multi>0
      // means SCC aggregation WOULD trigger). Ordering = plain Kahn over
      // he_adj; clusters unreachable due to cycles are appended in cluster
      // index order (legacy behavior, preference_disagreement.cpp L71-72).
      int H = (int)hyperedges.size();
      std::vector<int> order;
      if (H == 0) return order;

      std::vector<int> index(H, -1), lowlink(H, -1);
      std::vector<bool> on_stack(H, false);
      std::vector<int> stack;
      std::vector<int> scc_id(H, -1);
      int timer = 0, scc_count = 0;
      for (int i = 0; i < H; i++)
        if (index[i] == -1)
          tarjan_scc(i, scc_id, index, lowlink, on_stack, stack, timer, scc_count);
      std::vector<std::vector<int>> scc_members(scc_count);
      for (int i = 0; i < H; i++) scc_members[scc_id[i]].push_back(i);
      int n_multi = 0, max_size = 0;
      for (int i = 0; i < scc_count; i++) {
        if ((int)scc_members[i].size() > 1) n_multi++;
        max_size = std::max(max_size, (int)scc_members[i].size());
      }
      last_profile.n_sccs = scc_count;
      last_profile.n_multi_sccs = n_multi;
      last_profile.max_scc_size = max_size;
      HOTSTUFF_LOG_INFO("[[HyperGraph]] SCC stats: total=%d, multi=%d, max_size=%d",
                       scc_count, n_multi, max_size);

      std::queue<int> q;
      std::vector<int> indeg = he_indeg;
      for (int i = 0; i < H; i++) if (indeg[i] == 0) q.push(i);
      std::vector<bool> emitted(H, false);
      while (!q.empty()) {
        int u = q.front(); q.pop();
        emitted[u] = true; order.push_back(u);
        for (int v : he_adj[u]) if (--indeg[v] == 0) q.push(v);
      }
      int stalled = 0;
      for (int i = 0; i < H; i++)
        if (!emitted[i]) { order.push_back(i); stalled++; }
      last_profile.stalled_clusters = stalled;
      HOTSTUFF_LOG_INFO("[[HyperGraph-NOFB]] plain Kahn done, order=%lu stalled_clusters=%d",
                       order.size(), stalled);
      return order;
    }

public:
  HyperGraph() : margin(1.2), k_d(1.0), n_txns(0) {}

  std::vector<salticidae::uint256_t>
  finalize(std::vector<hotstuff::OrderedList> &orderedlists, int nfaulty,
           int nnodes, double gam = 1.0, double gen_param = 100.0,
           double network_param = 100.0, double measured_delay_ms = -1.0,
           double phi_override = -1.0) {
    last_profile = PhaseProfile{};
    // P derives delta from the certified preference matrix.  The legacy
    // timing arguments remain only for source compatibility and are ignored.
    (void)gen_param;
    (void)network_param;
    (void)measured_delay_ms;
    if (phi_override >= 0) k_d = std::max(0.0, phi_override);

    auto t0 = std::chrono::steady_clock::now();
    n_f = nfaulty;
    n_nodes = std::max(1, nnodes);
    gamma = gam;
    threshold = std::max(1, (int)(n_nodes * (1.0 - gamma)) + n_f + 1);

    compute_preferences(orderedlists);
    auto t1 = std::chrono::steady_clock::now();
    if (n_txns == 0)
      return {};
    if (n_txns == 1) {
      last_profile.n_hyperedges = 1;
      return {id_to_cmd[0]};
    }

    compute_canonical_positions();
    auto t2 = std::chrono::steady_clock::now();
    detect_clusters_refined();
    auto t3 = std::chrono::steady_clock::now();
    build_hypergraph();
    auto t4 = std::chrono::steady_clock::now();

    std::vector<int> order = topological_sort();
    auto t5 = std::chrono::steady_clock::now();

    auto us = [](auto a, auto b) {
      return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000.0;
    };
    last_profile.preferences_ms = us(t0, t1);
    last_profile.canonical_ms = us(t1, t2);
    last_profile.clusters_ms = us(t2, t3);
    last_profile.hypergraph_ms = us(t3, t4);
    last_profile.topo_sort_ms = us(t4, t5);
    last_profile.total_ms = us(t0, t5);

    HOTSTUFF_LOG_INFO("[[HyperGraph-P]] PROFILE K_D=%.3f prefs=%.2fms canon=%.2fms "
                     "clusters=%.2fms hypergraph=%.2fms topo=%.2fms total=%.2fms",
                     k_d, last_profile.preferences_ms, last_profile.canonical_ms,
                     last_profile.clusters_ms, last_profile.hypergraph_ms,
                     last_profile.topo_sort_ms, last_profile.total_ms);

    std::vector<salticidae::uint256_t> result;
    result.reserve(n_txns);
    for (int idx : order) {
      for (int tid : hyperedges[idx].internal_order) {
        result.push_back(id_to_cmd[tid]);
      }
    }
    return result;
  }

  const PhaseProfile& get_profile() const { return last_profile; }
  void set_k_d(double value) { k_d = std::max(0.0, value); }
  double get_k_d() const { return k_d; }
};

} // namespace FlashOrder

#endif
