#ifndef FLASHORDER_PREFERENCE_DISAGREEMENT_HPP
#define FLASHORDER_PREFERENCE_DISAGREEMENT_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

namespace flashorder_preference {

struct Result {
  std::vector<int> order;
  std::vector<std::vector<int>> clusters;
  double disagreement = 0.0;
  int delta = 0;
  int threshold = 1;
};

class Sorter {
 public:
  Sorter(int n_nodes, int f_byzantine, double gamma = 1.0, double k_d = 1.0,
         double margin = 1.2)
      : n_nodes_(n_nodes), f_byzantine_(f_byzantine), gamma_(gamma), k_d_(k_d),
        margin_(margin) {
    if (n_nodes <= 0 || f_byzantine < 0 || f_byzantine >= n_nodes)
      throw std::invalid_argument("invalid node/fault configuration");
    if (!std::isfinite(gamma) || gamma < 0.0 || gamma > 1.0)
      throw std::invalid_argument("gamma must be in [0, 1]");
    if (!std::isfinite(k_d) || k_d < 0.0)
      throw std::invalid_argument("k_d must be finite and non-negative");
    if (!std::isfinite(margin) || margin <= 1.0)
      throw std::invalid_argument("margin must be greater than one");
    threshold_ = std::max(1, static_cast<int>(std::floor(n_nodes * (1.0 - gamma))) + f_byzantine + 1);
  }

  static std::vector<std::vector<int>> preferences(const std::vector<std::vector<int>>& rows) {
    const std::size_t m = rows.empty() ? 0 : rows.front().size();
    std::vector<std::vector<int>> pref(m, std::vector<int>(m, 0));
    std::vector<bool> seen(m);
    for (const auto& row : rows) {
      if (row.size() != m) throw std::invalid_argument("rows have different lengths");
      std::fill(seen.begin(), seen.end(), false);
      for (std::size_t i = 0; i < m; ++i) {
        if (row[i] < 0 || row[i] >= static_cast<int>(m) || seen[row[i]])
          throw std::invalid_argument("rows must be permutations of 0..m-1");
        seen[row[i]] = true;
        for (std::size_t j = i + 1; j < m; ++j) ++pref[row[i]][row[j]];
      }
    }
    return pref;
  }

  static double disagreement(const std::vector<std::vector<int>>& pref) {
    const std::size_t m = pref.size();
    long long numerator = 0, denominator = 0;
    for (std::size_t i = 0; i < m; ++i) {
      if (pref[i].size() != m || pref[i][i] != 0) throw std::invalid_argument("invalid preference matrix");
      for (std::size_t j = i + 1; j < m; ++j) {
        if (pref[i][j] < 0 || pref[j][i] < 0) throw std::invalid_argument("negative preference count");
        numerator += 2LL * std::min(pref[i][j], pref[j][i]);
        denominator += pref[i][j] + pref[j][i];
      }
    }
    return denominator == 0 ? 0.0 : static_cast<double>(numerator) / denominator;
  }

  int logical_delta(const std::vector<std::vector<int>>& pref) const {
    if (pref.empty()) return 0;
    const double d = disagreement(pref);
    const int proposed = 1 + static_cast<int>(std::ceil(k_d_ * d));
    const int upper = std::max(1, static_cast<int>(pref.size()) - 1);
    return std::max(1, std::min(upper, proposed));
  }

  Result sort(const std::vector<std::vector<int>>& rows) const;
  int threshold() const { return threshold_; }

 private:
  int n_nodes_, f_byzantine_, threshold_;
  double gamma_, k_d_, margin_;
};

}  // namespace flashorder_preference

#endif
