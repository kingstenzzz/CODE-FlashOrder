#include "hotstuff/preference_disagreement.hpp"

namespace flashorder_preference {

Result Sorter::sort(const std::vector<std::vector<int>>& rows) const {
  Result result;
  result.threshold = threshold_;
  const auto pref = preferences(rows);
  const int m = static_cast<int>(pref.size());
  if (m == 0) return result;
  result.disagreement = disagreement(pref);
  result.delta = logical_delta(pref);

  std::vector<int> cp(m, 0), ids(m);
  for (int tx = 0; tx < m; ++tx) {
    for (int other = 0; other < m; ++other)
      if (other != tx && pref[other][tx] >= threshold_) ++cp[tx];
    ids[tx] = tx;
  }
  std::sort(ids.begin(), ids.end(), [&](int left, int right) {
    return std::pair<int, int>(cp[left], left) < std::pair<int, int>(cp[right], right);
  });
  result.clusters.push_back({ids.front()});
  for (std::size_t i = 1; i < ids.size(); ++i) {
    if (cp[ids[i]] - cp[ids[i - 1]] < result.delta)
      result.clusters.back().push_back(ids[i]);
    else
      result.clusters.push_back({ids[i]});
  }

  const int cluster_count = static_cast<int>(result.clusters.size());
  std::vector<std::vector<int>> internal(cluster_count);
  for (int c = 0; c < cluster_count; ++c) {
    internal[c] = result.clusters[c];
    std::sort(internal[c].begin(), internal[c].end(), [&](int left, int right) {
      long long left_score = 0, right_score = 0;
      for (int other : result.clusters[c]) {
        if (other == left) continue;
        left_score += pref[left][other] - pref[other][left];
      }
      for (int other : result.clusters[c]) {
        if (other == right) continue;
        right_score += pref[right][other] - pref[other][right];
      }
      return std::pair<long long, int>(-left_score, left) < std::pair<long long, int>(-right_score, right);
    });
  }

  std::vector<std::vector<int>> adj(cluster_count);
  std::vector<int> indegree(cluster_count, 0);
  for (int left = 0; left < cluster_count; ++left) {
    for (int right = left + 1; right < cluster_count; ++right) {
      long long forward = 0, reverse = 0;
      for (int a : result.clusters[left]) for (int b : result.clusters[right]) {
        forward += pref[a][b];
        reverse += pref[b][a];
      }
      if (forward > margin_ * reverse) { adj[left].push_back(right); ++indegree[right]; }
      else if (reverse > margin_ * forward) { adj[right].push_back(left); ++indegree[left]; }
    }
  }
  std::priority_queue<int, std::vector<int>, std::greater<int>> ready;
  for (int c = 0; c < cluster_count; ++c) if (indegree[c] == 0) ready.push(c);
  std::vector<bool> emitted(cluster_count, false);
  while (!ready.empty()) {
    int c = ready.top(); ready.pop();
    emitted[c] = true;
    result.order.insert(result.order.end(), internal[c].begin(), internal[c].end());
    for (int target : adj[c]) if (--indegree[target] == 0) ready.push(target);
  }
  for (int c = 0; c < cluster_count; ++c)
    if (!emitted[c]) result.order.insert(result.order.end(), internal[c].begin(), internal[c].end());
  return result;
}

}  // namespace flashorder_preference
