#ifndef _AUNCEL_AUNCEL_H
#define _AUNCEL_AUNCEL_H

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include "hotstuff/type.h"

namespace hotstuff {

class WeightOrderer {
public:
    static inline std::vector<uint256_t> weight_order_phase(
        const std::vector<std::vector<uint256_t>>& local_orderings,
        double k);

    static inline std::vector<uint256_t> final_order_phase(
        const std::vector<uint256_t>& linearized);

    static inline std::vector<uint256_t> order(
        const std::vector<std::vector<uint256_t>>& local_orderings,
        double k = 0.5);

    static inline std::vector<uint256_t> linearize_dag(
        const std::unordered_map<uint256_t, std::unordered_set<uint256_t>>& dag);
};

inline std::vector<uint256_t> WeightOrderer::weight_order_phase(
    const std::vector<std::vector<uint256_t>>& local_orderings,
    double k) {
    if (local_orderings.empty()) return {};
    if (k <= 0.0 || k >= 1.0) k = 0.5;

    std::unordered_map<uint256_t, double> weight_sum;
    weight_sum.reserve(64);

    double k_pow = 1.0;
    std::vector<double> pow_cache;
    pow_cache.reserve(64);
    for (size_t d = 0; d < 64; d++) {
        pow_cache.push_back(k_pow);
        k_pow *= k;
    }

    for (const auto& ordering : local_orderings) {
        size_t d = 1;
        for (auto it = ordering.begin(); it != ordering.end(); ++it, ++d) {
            double w = (d < pow_cache.size()) ? (1.0 - pow_cache[d]) : (1.0 - std::pow(k, static_cast<double>(d)));
            weight_sum[*it] += w;
        }
    }

    std::vector<uint256_t> result;
    result.reserve(weight_sum.size());
    for (const auto& kv : weight_sum) result.push_back(kv.first);

    std::sort(result.begin(), result.end(),
        [&weight_sum](const uint256_t& a, const uint256_t& b) {
            double wa = weight_sum[a];
            double wb = weight_sum[b];
            if (wa != wb) return wa < wb;
            return a < b;
        });

    return result;
}

inline std::vector<uint256_t> WeightOrderer::final_order_phase(
    const std::vector<uint256_t>& linearized) {
    if (linearized.size() <= 1) return linearized;

    size_t median_idx = linearized.size() / 2;
    uint256_t median = linearized[median_idx];

    std::vector<uint256_t> pre_sequence(linearized.begin(), linearized.begin() + median_idx);
    std::vector<uint256_t> post_sequence(linearized.begin() + median_idx + 1, linearized.end());

    auto pre_sorted = final_order_phase(pre_sequence);
    auto post_sorted = final_order_phase(post_sequence);

    std::vector<uint256_t> result;
    result.reserve(pre_sorted.size() + 1 + post_sorted.size());
    result.insert(result.end(), pre_sorted.begin(), pre_sorted.end());
    result.push_back(median);
    result.insert(result.end(), post_sorted.begin(), post_sorted.end());

    return result;
}

inline std::vector<uint256_t> WeightOrderer::order(
    const std::vector<std::vector<uint256_t>>& local_orderings,
    double k) {
    auto linearized = weight_order_phase(local_orderings, k);
    return final_order_phase(linearized);
}

inline std::vector<uint256_t> WeightOrderer::linearize_dag(
    const std::unordered_map<uint256_t, std::unordered_set<uint256_t>>& dag) {
    std::vector<std::pair<size_t, uint256_t>> keyed;
    keyed.reserve(dag.size());
    for (const auto& kv : dag) {
        keyed.emplace_back(kv.second.size(), kv.first);
    }
    std::sort(keyed.begin(), keyed.end(),
        [](const std::pair<size_t, uint256_t>& a,
           const std::pair<size_t, uint256_t>& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });
    std::vector<uint256_t> result;
    result.reserve(keyed.size());
    for (auto& kv : keyed) result.push_back(kv.second);
    return result;
}

}

#endif
