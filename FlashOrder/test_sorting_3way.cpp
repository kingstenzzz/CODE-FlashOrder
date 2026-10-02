// Auncel vs FlashOrder vs Themis Sorting Benchmark (3-way)
#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <random>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <numeric>
#include "hotstuff/flashorder.h"
#include "hotstuff/graph.h"
#include "hotstuff/auncel.h"

using namespace hotstuff;
using namespace FlashOrder;

struct BenchmarkResult {
    double total_time_ms;
    std::vector<uint256_t> result;
};

std::vector<OrderedList> generate_random_orderings(int n_nodes, int n_txns, uint32_t seed, double discrepancy) {
    std::mt19937 rng(seed);
    std::vector<OrderedList> orderings(n_nodes);

    std::vector<int> base_indices(n_txns);
    std::iota(base_indices.begin(), base_indices.end(), 0);
    std::shuffle(base_indices.begin(), base_indices.end(), rng);

    int n_swap = static_cast<int>(n_txns * discrepancy);

    for (int r = 0; r < n_nodes; r++) {
        auto indices = base_indices;
        if (r > 0 && n_swap > 0) {
            for (int s = 0; s < n_swap; s++) {
                int i = rng() % n_txns;
                int j = rng() % n_txns;
                std::swap(indices[i], indices[j]);
            }
        }
        orderings[r].cmds.reserve(n_txns);
        orderings[r].timestamps.reserve(n_txns);
        for (int i = 0; i < n_txns; i++) {
            uint256_t cmd;
            memset(&cmd, indices[i], sizeof(cmd));
            orderings[r].cmds.push_back(cmd);
            orderings[r].timestamps.push_back(i * 100);
        }
    }
    return orderings;
}

std::vector<std::vector<uint256_t>> convert_to_vectors(const std::vector<OrderedList>& orderings) {
    std::vector<std::vector<uint256_t>> result;
    for (const auto& ol : orderings) result.push_back(ol.cmds);
    return result;
}

BenchmarkResult auncel_benchmark(const std::vector<std::vector<uint256_t>>& orderings, double k) {
    BenchmarkResult res;
    auto start = std::chrono::steady_clock::now();
    res.result = WeightOrderer::order(orderings, k);
    auto end = std::chrono::steady_clock::now();
    res.total_time_ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    return res;
}

BenchmarkResult themis_benchmark(const std::vector<std::vector<uint256_t>>& orderings) {
    BenchmarkResult res;
    auto start = std::chrono::steady_clock::now();

    std::unordered_map<uint256_t, std::unordered_set<uint256_t>> graph;
    for (const auto& ol : orderings) {
        for (size_t i = 0; i < ol.size(); i++) {
            for (size_t j = i + 1; j < ol.size(); j++) {
                graph[ol[i]].insert(ol[j]);
            }
        }
    }
    if (!graph.empty()) {
        CondensationGraph condgraph(graph);
        auto sccs = condgraph.get_condensation_graph();
        for (const auto& scc : sccs) {
            for (const auto& cmd : scc) res.result.push_back(cmd);
        }
    }
    auto end = std::chrono::steady_clock::now();
    res.total_time_ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    return res;
}

BenchmarkResult flashorder_benchmark(std::vector<OrderedList>& orderings,
                                      int n_faulty, int n_nodes, double gamma) {
    BenchmarkResult res;
    auto start = std::chrono::steady_clock::now();
    HyperGraph<10000> hg;
    res.result = hg.finalize(orderings, n_faulty, n_nodes, gamma);
    auto end = std::chrono::steady_clock::now();
    res.total_time_ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    return res;
}

double compute_rbo(const std::vector<uint256_t>& a, const std::vector<uint256_t>& b, double p = 0.85) {
    if (a.empty() && b.empty()) return 1.0;
    if (a.empty() || b.empty()) return 0.0;

    std::unordered_map<uint256_t, int> pos_b;
    for (size_t i = 0; i < b.size(); i++) pos_b[b[i]] = static_cast<int>(i);

    size_t max_depth = std::min(a.size(), b.size());
    double rbo = 0.0;
    int overlap = 0;
    double p_pow = 1.0;

    std::unordered_set<uint256_t> seen_a, seen_b;
    for (size_t d = 1; d <= max_depth; d++) {
        seen_a.insert(a[d-1]);
        seen_b.insert(b[d-1]);
        int common = 0;
        for (const auto& x : seen_a) if (seen_b.count(x)) common++;
        overlap = common;
        p_pow *= p;
        rbo += (p_pow / d) * overlap;
    }
    return rbo * (1.0 - p);
}

void print_separator() {
    std::cout << "=============================================================================================================\n";
}

void run_benchmark(int n_nodes, int n_txns, int n_faulty, double gamma, double auncel_k, double discrepancy, uint32_t seed) {
    std::cout << "\n--- nodes=" << n_nodes << " txns=" << n_txns
              << " f=" << n_faulty << " gamma=" << gamma
              << " k=" << auncel_k << " disc=" << discrepancy << " seed=" << seed << " ---\n";

    auto orderings = generate_random_orderings(n_nodes, n_txns, seed, discrepancy);
    auto orderings_vectors = convert_to_vectors(orderings);
    auto orderings_copy = orderings;

    auto au_res = auncel_benchmark(orderings_vectors, auncel_k);
    auto them_res = themis_benchmark(orderings_vectors);
    auto fo_res = flashorder_benchmark(orderings_copy, n_faulty, n_nodes, gamma);

    double rbo_au_fo = compute_rbo(au_res.result, fo_res.result);
    double rbo_au_them = compute_rbo(au_res.result, them_res.result);
    double rbo_fo_them = compute_rbo(fo_res.result, them_res.result);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  Auncel     : " << au_res.total_time_ms << " ms  (size=" << au_res.result.size() << ")\n";
    std::cout << "  FlashOrder : " << fo_res.total_time_ms << " ms  (size=" << fo_res.result.size() << ")\n";
    std::cout << "  Themis     : " << them_res.total_time_ms << " ms  (size=" << them_res.result.size() << ")\n";

    std::cout << std::setprecision(4);
    std::cout << "  RBO Auncel↔FlashOrder : " << rbo_au_fo << "\n";
    std::cout << "  RBO Auncel↔Themis     : " << rbo_au_them << "\n";
    std::cout << "  RBO FlashOrder↔Themis : " << rbo_fo_them << "\n";

    std::cout << std::setprecision(2);
    if (them_res.total_time_ms > 0.001 && au_res.total_time_ms > 0.001) {
        std::cout << "  Speedup vs Themis -> Auncel: " << (them_res.total_time_ms / au_res.total_time_ms) << "x"
                  << "  FlashOrder: " << (them_res.total_time_ms / fo_res.total_time_ms) << "x\n";
    }
}

int main(int argc, char** argv) {
    std::cout << "=== Auncel vs FlashOrder vs Themis Sorting Benchmark ===\n";
    print_separator();

    std::vector<std::tuple<int, int, int, double, double, double, uint32_t>> test_cases;

    std::cout << "\n>>> Phase 1: Vary block size (n=4 nodes, f=1) <<<\n";
    for (int n_txns : {50, 100, 200, 500, 1000}) {
        test_cases.emplace_back(4, n_txns, 1, 0.75, 0.5, 0.1, 42);
    }

    std::cout << "\n>>> Phase 2: Vary network scale (block_size=100) <<<\n";
    for (int n_nodes : {4, 9, 16, 25}) {
        int f = (n_nodes - 1) / 4;
        test_cases.emplace_back(n_nodes, 100, f, 0.75, 0.5, 0.1, 42);
    }

    std::cout << "\n>>> Phase 3: Vary discrepancy rate (n=4, block_size=100) <<<\n";
    for (double disc : {0.1, 0.3, 0.5}) {
        test_cases.emplace_back(4, 100, 1, 0.75, 0.5, disc, 42);
    }

    std::cout << "\n>>> Phase 4: Vary Auncel k parameter (n=4, block_size=100) <<<\n";
    for (double k : {0.25, 0.5, 0.75}) {
        test_cases.emplace_back(4, 100, 1, 0.75, k, 0.1, 42);
    }

    for (const auto& tc : test_cases) {
        int n_nodes, n_txns, n_faulty;
        double gamma, k, disc;
        uint32_t seed;
        std::tie(n_nodes, n_txns, n_faulty, gamma, k, disc, seed) = tc;
        run_benchmark(n_nodes, n_txns, n_faulty, gamma, k, disc, seed);
    }

    print_separator();
    std::cout << "=== Benchmark Complete ===\n";
    return 0;
}
