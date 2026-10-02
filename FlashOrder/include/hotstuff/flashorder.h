#ifndef _FLASHORDER_H
#define _FLASHORDER_H

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
#include "hotstuff/stage_profiling.h"

namespace FlashOrder {

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
    double scc_ms;        // [R2-D5] tarjan + condensation only
    double ordering_ms;   // [R2-D5] kahn + order_within_scc only
    double topo_sort_ms;
    double total_ms;
    int delta;
    int n_hyperedges;
    int n_sccs;
    int n_multi_sccs;
    int max_scc_size;
    // strict_repair telemetry (env FLASHORDER_P_STRICT_REPAIR=1)
    int repair_rounds = 0;   // 端点合并迭代轮数(0 = off 或无违例)
    int repair_merges = 0;   // 实际发生的组合并次数
    // 最终输出序的组(batch)边界: 每组交易数, 供吸收语义的指标计算
    std::vector<int> final_group_sizes;
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
                  [](const auto &a, const auto &b) {
                    if (a.first != b.first) return a.first > b.first;
                    return a.second < b.second;   // deterministic tie-break
                  });
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
      int H = (int)hyperedges.size();
      std::vector<int> order;
      if (H == 0)
        return order;

      /* [R2-D5] SCC phase: tarjan + condensation */
      auto _scc_t0 = STAGE_NOW();
      std::vector<int> index(H, -1), lowlink(H, -1);
      std::vector<bool> on_stack(H, false);
      std::vector<int> stack;
      std::vector<int> scc_id(H, -1);
      int timer = 0, scc_count = 0;

      for (int i = 0; i < H; i++) {
        if (index[i] == -1)
          tarjan_scc(i, scc_id, index, lowlink, on_stack, stack, timer, scc_count);
      }

      std::vector<std::vector<int>> scc_adj;
      std::vector<int> scc_indeg;
      build_scc_condensation(scc_id, scc_count, scc_adj, scc_indeg);
      STAGE_PROF("scc", 0, (size_t)H, STAGE_US(_scc_t0));

      /* [R2-D5] Ordering phase: member grouping + kahn + within-SCC order */
      auto _ord_t0 = STAGE_NOW();

      std::vector<std::vector<int>> scc_members(scc_count);
      for (int i = 0; i < H; i++)
        scc_members[scc_id[i]].push_back(i);

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
      for (int i = 0; i < scc_count; i++)
        if (scc_indeg[i] == 0)
          q.push(i);

      /* Kahn: collect SCC groups in topological order. */
      std::vector<int> group_seq;
      while (!q.empty()) {
        int u = q.front();
        q.pop();
        group_seq.push_back(u);
        for (int v : scc_adj[u]) {
          if (--scc_indeg[v] == 0)
            q.push(v);
        }
      }

      /* strict_repair (optional, default OFF; env FLASHORDER_P_STRICT_REPAIR=1).
       * Faithful implementation of Algorithm 2 (StrictRepair):
       *   while some admitted pair (txi,txj) in R_q(C) lies in distinct
       *   clusters with no quotient path Hi -> Hj:
       *     - merge endpoint clusters of the lexicographically smallest
       *       violating tuple (key(Hi), key(Hj), id(txi), id(txj));
       *       - rebuild all quotient edges from P;
       *       - fold each minimum-key nontrivial quotient-SCC into a single
       *         cluster until the quotient graph is acyclic.
       * key(H) = min member txn id (same as CCOF-R cluster_key). */
      static int strict_repair_on = -1;
      if (strict_repair_on < 0)
        strict_repair_on = std::getenv("FLASHORDER_P_STRICT_REPAIR") ? 1 : 0;
      bool strict_handled = false;
      if (strict_repair_on && (int)hyperedges.size() > 1) {
        strict_handled = true;
        auto _repair_t0 = STAGE_NOW();
        /* ===== [OPT-1] cheap pre-check: when the quotient is already acyclic
         * and no admitted pair (pref >= threshold) lies in two distinct
         * clusters without a quotient path, there is nothing to repair.
         * Skip the whole strict machinery and fall back to the normal path. */
        {
          int H0 = (int)hyperedges.size();
          std::vector<int> e_idx(H0, -1), e_low(H0, -1), e_sid(H0, -1), e_stk;
          std::vector<bool> e_onstk(H0, false);
          int e_timer = 0, e_ncomp = 0;
          for (int i = 0; i < H0; i++)
            if (e_idx[i] == -1)
              tarjan_scc(i, e_sid, e_idx, e_low, e_onstk, e_stk, e_timer, e_ncomp);
          bool acyclic = (e_ncomp == H0);
          bool clean = acyclic;
          if (acyclic && H0 > 1) {
            int Wd = (H0 + 63) / 64;
            std::vector<std::vector<uint64_t>> RR(H0, std::vector<uint64_t>(Wd, 0));
            std::vector<int> indeg = he_indeg;
            std::vector<int> topo; topo.reserve(H0);
            std::vector<int> qq;
            for (int i = 0; i < H0; i++) if (!indeg[i]) qq.push_back(i);
            while (!qq.empty()) { int u = qq.back(); qq.pop_back(); topo.push_back(u);
              for (int v : he_adj[u]) if (!--indeg[v]) qq.push_back(v); }
            for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
              int u = *it; auto &ru = RR[u];
              for (int v : he_adj[u]) {
                ru[v >> 6] |= 1ULL << (v & 63);
                const auto &rv = RR[v];
                for (int w = 0; w < Wd; w++) ru[w] |= rv[w];
              }
            }
            std::vector<int> tc(n_txns, -1);
            for (int h = 0; h < H0; h++)
              for (int t : hyperedges[h].members) tc[t] = h;
            for (int a = 0; a < n_txns && clean; a++) {
              int ca = tc[a]; if (ca < 0) continue;
              const uint16_t *row = &pref[(size_t)a * n_txns];
              for (int b = 0; b < n_txns; b++) {
                if (b == a) continue;
                int cb = tc[b]; if (cb < 0 || cb == ca) continue;
                if ((int)row[b] >= threshold &&
                    !((RR[ca][cb >> 6] >> (cb & 63)) & 1ULL)) {
                  clean = false; break;
                }
              }
            }
          }
          if (clean) { strict_handled = false; goto strict_bypass; }
        }
        /* ===== /[OPT-1] ===== */
        auto he_key = [&](int h) {
          return *std::min_element(hyperedges[h].members.begin(),
                                   hyperedges[h].members.end());
        };
        // recompute internal (net-score) order of a merged cluster
        auto recompute_internal = [&](HyperEdge &he) {
          if (he.members.size() <= 1) {
            he.internal_order = he.members;
            return;
          }
          std::vector<std::pair<long long, int>> scores;
          for (int m : he.members) {
            long long net = 0;
            int off_m = m * n_txns;
            for (int o : he.members)
              if (o != m)
                net += (long long)pref[off_m + o] - (long long)pref[o * n_txns + m];
            scores.push_back({net, m});
          }
          std::sort(scores.begin(), scores.end(),
                    [](const auto &a, const auto &b) {
                      if (a.first != b.first) return a.first > b.first;
                      return a.second < b.second;
                    });
          he.internal_order.clear();
          for (auto &p : scores) he.internal_order.push_back(p.second);
        };
        // cluster-pair weight matrix Wm[i][j] = sum_{a in i, b in j} pref[a][b]:
        // built once cache-friendly, then maintained incrementally on merges.
        // nzr[i] = sorted list of clusters j with nonzero weight to i (sparse
        // counterpart index) — adjacency derivation touches only nonzero
        // pairs instead of scanning all H^2. No per-merge re-sort (nothing
        // consumes vector order mid-repair; selection uses explicit keys);
        // clusters are sorted by key once before final linearization.
        std::vector<std::vector<long long>> Wm;
        std::vector<std::vector<int>> nzr;
        // transitive-closure bitmatrix R[i] over the quotient graph,
        // maintained incrementally across merges (exact: any merge that
        // makes a direct edge vanish falls back to a full recompute)
        std::vector<std::vector<uint64_t>> R;
        auto compute_closure = [&]() {
          // exact transitive closure of the (acyclic) quotient graph:
          // Kahn topological order, then reverse propagation
          int H = (int)hyperedges.size();
          int Wd = (H + 63) / 64;
          R.assign(H, std::vector<uint64_t>(Wd, 0));
          std::vector<int> indeg = he_indeg;
          std::vector<int> q;
          for (int i = 0; i < H; i++) if (!indeg[i]) q.push_back(i);
          std::vector<int> topo;
          while (!q.empty()) {
            int u = q.back(); q.pop_back();
            topo.push_back(u);
            for (int v : he_adj[u]) if (!--indeg[v]) q.push_back(v);
          }
          for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
            int u = *it;
            auto &ru = R[u];
            for (int v : he_adj[u]) {
              ru[v >> 6] |= 1ULL << (v & 63);
              const auto &rv = R[v];
              for (int w = 0; w < Wd; w++) ru[w] |= rv[w];
            }
          }
        };
        auto build_W = [&]() {
          int H = (int)hyperedges.size();
          Wm.assign(H, std::vector<long long>(H, 0));
          std::vector<int> tcl(n_txns, -1);
          for (int h = 0; h < H; h++)
            for (int t : hyperedges[h].members) tcl[t] = h;
          for (int i = 0; i < H; i++) {
            auto &Wi = Wm[i];
            for (int a : hyperedges[i].members) {
              const auto *row = &pref[(size_t)a * n_txns];
              for (int b = 0; b < n_txns; b++) Wi[tcl[b]] += row[b];
            }
          }
          nzr.assign(H, {});
          for (int i = 0; i < H; i++)
            for (int j = i + 1; j < H; j++)
              if ((Wm[i][j] | Wm[j][i]) != 0) {
                nzr[i].push_back(j);
                nzr[j].push_back(i);
              }
        };
        // merge clusters `idxs` into one (kept at idxs min): updates
        // hyperedges, Wm, nzr. O(deg) instead of O(H^2).
        auto merge_clusters = [&](std::vector<int> idxs) {
          std::sort(idxs.begin(), idxs.end());
          int H = (int)hyperedges.size();
          HyperEdge merged;
          for (int h : idxs)
            merged.members.insert(merged.members.end(),
                                  hyperedges[h].members.begin(),
                                  hyperedges[h].members.end());
          std::sort(merged.members.begin(), merged.members.end());
          merged.members.erase(std::unique(merged.members.begin(),
                                           merged.members.end()),
                               merged.members.end());
          recompute_internal(merged);
          const int keep = idxs[0];
          // union of counterpart lists, minus merged members
          std::vector<int> S;
          for (int h : idxs) S.insert(S.end(), nzr[h].begin(), nzr[h].end());
          std::sort(S.begin(), S.end());
          S.erase(std::unique(S.begin(), S.end()), S.end());
          S.erase(std::remove_if(S.begin(), S.end(), [&](int x) {
                    return std::binary_search(idxs.begin(), idxs.end(), x);
                  }), S.end());
          // Wm row/col for keep
          std::vector<long long> row(H, 0), col(H, 0);
          for (int h : idxs)
            for (int x = 0; x < H; x++) {
              row[x] += Wm[h][x];
              col[x] += Wm[x][h];
            }
          hyperedges[keep] = std::move(merged);
          Wm[keep] = std::move(row);
          for (int x = 0; x < H; x++) Wm[x][keep] = col[x];
          Wm[keep][keep] = 0;
          nzr[keep] = std::move(S);
          // fix counterpart lists of neighbors: drop merged idxs, add keep
          for (int x : nzr[keep]) {
            auto &L = nzr[x];
            L.erase(std::remove_if(L.begin(), L.end(), [&](int y) {
                      return std::binary_search(idxs.begin(), idxs.end(), y);
                    }), L.end());
            if (std::find(L.begin(), L.end(), keep) == L.end())
              L.insert(std::lower_bound(L.begin(), L.end(), keep), keep);
          }
          // erase dropped clusters (descending index)
          for (auto it = idxs.rbegin(); it != idxs.rend(); ++it) {
            if (*it == keep) continue;
            hyperedges.erase(hyperedges.begin() + *it);
            Wm.erase(Wm.begin() + *it);
            nzr.erase(nzr.begin() + *it);
            for (auto &r : Wm) r.erase(r.begin() + *it);
            // remap indices above the erased one in every nzr row
            for (auto &L : nzr)
              for (int &y : L)
                if (y > *it) y--;
          }
          // ---- incremental adjacency: only edges incident to `keep` can
          // change (other W values unchanged), so fix he_adj/he_indeg in
          // O(deg) instead of an O(E) rebuild ----
          {
            // (a) in OLD index space: strip every edge touching merged idxs
            std::vector<char> inM(H, 0);
            for (int h : idxs) inM[h] = 1;
            for (int h : idxs) {
              for (int v : he_adj[h]) he_indeg[v]--;  // out-edge h->v vanishes
              he_adj[h].clear();
            }
            for (int x = 0; x < H; x++)
              if (!inM[x])
                for (int h : idxs) {
                  // in-edge x->h vanishes (h's row is dropped anyway)
                  auto &L = he_adj[x];
                  L.erase(std::remove(L.begin(), L.end(), h), L.end());
                }
            // (b) erase rows of dropped idxs, remap remaining indices
            for (auto it = idxs.rbegin(); it != idxs.rend(); ++it) {
              if (*it == keep) continue;
              he_adj.erase(he_adj.begin() + *it);
              he_indeg.erase(he_indeg.begin() + *it);
              int e = *it;
              for (auto &L : he_adj)
                for (int &y : L)
                  if (y > e) y--;
            }
            // (c) fresh edges for keep from current Wm via nzr
            he_adj[keep].clear();
            int keep_deg = he_indeg[keep];
            he_indeg[keep] = 0;
            for (int x : nzr[keep]) {
              const long long s_ij = Wm[keep][x], s_ji = Wm[x][keep];
              if (s_ij > margin * (double)s_ji) {
                he_adj[keep].push_back(x); he_indeg[x]++;
              } else if (s_ji > margin * (double)s_ij) {
                he_adj[x].push_back(keep); he_indeg[keep]++;
              }
            }
            (void)keep_deg;
          }
          // (closure R is recomputed lazily per round; merges do not
          //  maintain it)
        };
        // quotient edges from Wm via sparse nzr (margin rule)
        auto rebuild_edges = [&]() {
          int H = (int)hyperedges.size();
          he_adj.assign(H, std::vector<int>());
          he_indeg.assign(H, 0);
          for (int i = 0; i < H; i++)
            for (int j : nzr[i]) {
              if (j <= i) continue;
              const long long s_ij = Wm[i][j], s_ji = Wm[j][i];
              if (s_ij > margin * (double)s_ji) {
                he_adj[i].push_back(j); he_indeg[j]++;
              } else if (s_ji > margin * (double)s_ij) {
                he_adj[j].push_back(i); he_indeg[i]++;
              }
            }
        };
        // fold min-key nontrivial quotient-SCCs until acyclic
        auto fold_sccs = [&]() {
          int folds = 0;
          for (;;) {
            int H = (int)hyperedges.size();
            if (H <= 1) break;
            std::vector<int> index(H, -1), low(H, -1), sid(H, -1), stk;
            std::vector<bool> onstk(H, false);
            int timer = 0, ncomp = 0;
            for (int i = 0; i < H; i++)
              if (index[i] == -1)
                tarjan_scc(i, sid, index, low, onstk, stk, timer, ncomp);
            if (ncomp == H) break;  // all singletons => acyclic
            // pick nontrivial SCC with min key
            int best = -1, best_key = INT32_MAX;
            std::vector<std::vector<int>> comps(ncomp);
            for (int i = 0; i < H; i++) comps[sid[i]].push_back(i);
            for (int c = 0; c < ncomp; c++) {
              if (comps[c].size() <= 1) continue;
              int key = INT32_MAX;
              for (int h : comps[c]) key = std::min(key, he_key(h));
              if (key < best_key) { best_key = key; best = c; }
            }
            if (best < 0) break;
            merge_clusters(comps[best]);  // adjacency maintained incrementally
            folds++;
          }
          return folds;
        };
        // initial fold (equivalent to quotient-SCC closure)
        build_W();
        rebuild_edges();
        last_profile.repair_merges += fold_sccs();
        /* admitted pairs (pref >= threshold) depend only on P, not on the
         * partition: precompute once, reuse every round. Cuts the per-round
         * violation scan from O(n^2) to O(|admitted|). */
        std::vector<std::pair<int, int>> admitted;
        for (int a = 0; a < n_txns; a++) {
          int off_a = a * n_txns;
          for (int b = 0; b < n_txns; b++)
            if (b != a && (int)pref[off_a + b] >= threshold)
              admitted.push_back({a, b});
        }
        /* lazy per-source reachability over the quotient DAG, bitset BFS.
         * Semantically identical to a full transitive closure; word-parallel
         * frontier propagation: O(K * H * H/64) instead of O(H^3). */
        for (int round = 1;; round++) {
          // txn -> cluster
          int H = (int)hyperedges.size();
          std::vector<int> tc(n_txns, -1);
          for (int h = 0; h < H; h++)
            for (int t : hyperedges[h].members) tc[t] = h;
          // exact closure of the current (post-fold, acyclic) quotient graph
          compute_closure();
          // reachability via the closure R
          // lexicographically smallest violating tuple
          int bi = -1, bj = -1, bti = -1, btj = -1;
          long long bkey = LLONG_MAX;
          for (const auto &pr : admitted) {
            const int a = pr.first, b = pr.second;
            int ca = tc[a], cb = tc[b];
            if (ca < 0 || cb < 0 || ca == cb) continue;
            if ((R[ca][cb >> 6] >> (cb & 63)) & 1ULL) continue;  // forward path
            long long key = (long long)he_key(ca) * 1000000000LL + he_key(cb);
            if (key < bkey || (key == bkey && a < bti) ||
                (key == bkey && a == bti && b < btj)) {
              bkey = key; bi = ca; bj = cb; bti = a; btj = b;
            }
          }
          if (bi < 0) break;
          last_profile.repair_rounds = round;
          // merge endpoint clusters (updates hyperedges + Wm, sorts by key)
          merge_clusters({bi, bj});  // adjacency maintained incrementally
          last_profile.repair_merges++;
          // fold newly created cycles until acyclic
          last_profile.repair_merges += fold_sccs();
        }
        HOTSTUFF_LOG_INFO("[[HyperGraph]] strict_repair: rounds=%d merges=%d clusters=%lu",
                          last_profile.repair_rounds, last_profile.repair_merges,
                          hyperedges.size());
        /* strict path: sort clusters by key once, linearize via Kahn */
        order.clear();
        {
          int H2 = (int)hyperedges.size();
          std::vector<int> perm(H2);
          for (int i = 0; i < H2; i++) perm[i] = i;
          std::sort(perm.begin(), perm.end(), [&](int x, int y) {
            return *std::min_element(hyperedges[x].members.begin(),
                                     hyperedges[x].members.end()) <
                   *std::min_element(hyperedges[y].members.begin(),
                                     hyperedges[y].members.end());
          });
          std::vector<HyperEdge> he2;
          he2.reserve(H2);
          std::vector<std::vector<long long>> w2(H2);
          std::vector<std::vector<int>> nz2(H2);
          for (int i = 0; i < H2; i++) {
            he2.push_back(std::move(hyperedges[perm[i]]));
            w2[i] = std::move(Wm[perm[i]]);
            nz2[i] = std::move(nzr[perm[i]]);
          }
          // remap nzr indices old -> new (inverse permutation)
          std::vector<int> inv(H2);
          for (int i = 0; i < H2; i++) inv[perm[i]] = i;
          for (int i = 0; i < H2; i++)
            for (int &y : nz2[i]) y = inv[y];
          // permute Wm columns old -> new as well
          for (int i = 0; i < H2; i++) {
            std::vector<long long> r(H2);
            for (int j = 0; j < H2; j++) r[inv[j]] = w2[i][j];
            w2[i] = std::move(r);
          }
          hyperedges = std::move(he2);
          Wm = std::move(w2);
          nzr = std::move(nz2);
        }
        rebuild_edges();
        int H = (int)hyperedges.size();
        std::vector<int> indeg = he_indeg;
        std::queue<int> rq;
        for (int i = 0; i < H; i++)
          if (indeg[i] == 0) rq.push(i);
        while (!rq.empty()) {
          int u = rq.front(); rq.pop();
          order.push_back(u);
          for (int v : he_adj[u])
            if (--indeg[v] == 0) rq.push(v);
        }
        STAGE_PROF("repair", 0, (size_t)hyperedges.size(), STAGE_US(_repair_t0));
        STAGE_PROF("repair_merges", 0, (size_t)last_profile.repair_merges, 0.0);
      }
strict_bypass:

      if (!strict_handled) {
        for (int u : group_seq) {
          auto inner = order_within_scc(scc_members[u]);
          for (int tx : inner)
            order.push_back(tx);
        }
      }

      /* final group boundaries (in txns) for absorption-aware metrics:
       * order is a sequence of hyperedge indices on both paths */
      last_profile.final_group_sizes.clear();
      for (int idx : order)
        last_profile.final_group_sizes.push_back(
            (int)hyperedges[idx].members.size());

      HOTSTUFF_LOG_INFO("[[HyperGraph]] topological_sort DONE (SCC merged), order=%lu", order.size());
      STAGE_PROF("ordering", 0, (size_t)order.size(), STAGE_US(_ord_t0));
      return order;
    }

public:
  /* read-only accessors for instrumentation (lambda sensitivity studies) */
  const std::vector<HyperEdge> &get_hyperedges() const { return hyperedges; }
  long pref_at(int a, int b) const { return pref[a * n_txns + b]; }
  int get_n_txns() const { return n_txns; }

  HyperGraph() : margin(1.2), k_d(1.0), n_txns(0) {
    /* inter-cluster margin lambda is configurable for sensitivity studies;
       default 1.2 preserves the original built-in semantics */
    if (const char *m = std::getenv("FLASHORDER_P_MARGIN"))
      margin = std::max(1.0, atof(m));
  }

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

    /* [R2-D5] canonical 5-stage mapping (deferred, in us):
     *  graph_construction = preferences + canonical + hypergraph build
     *  batching           = delta clustering (batch/hyperedge formation)
     *  scc / ordering     = emitted inside topological_sort()
     */
    last_profile.scc_ms = 0; // filled by topological_sort STAGE_PROF
    last_profile.ordering_ms = 0;
    STAGE_PROF("graph_construction", 0, (size_t)n_txns,
               1000.0 * (last_profile.preferences_ms + last_profile.canonical_ms + last_profile.hypergraph_ms));
    STAGE_PROF("batching", 0, (size_t)n_txns, 1000.0 * last_profile.clusters_ms);
    /* [R2-D5] finer stage breakdown (matches the response's FlashOrder stages) */
    STAGE_PROF("preferences", 0, (size_t)n_txns, 1000.0 * last_profile.preferences_ms);
    STAGE_PROF("canonical", 0, (size_t)n_txns, 1000.0 * last_profile.canonical_ms);
    STAGE_PROF("hypergraph", 0, (size_t)n_txns, 1000.0 * last_profile.hypergraph_ms);

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
