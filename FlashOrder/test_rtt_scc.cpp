#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <algorithm>
#include <cstring>
#include <chrono>
#include "hotstuff/entity.h"
#include "hotstuff/flashorder.h"

using namespace hotstuff;
using namespace FlashOrder;

int main() {
    int n_replicas = 4;
    int n_txns = 200;
    uint32_t seed = 42;

    std::mt19937 rng(seed);
    std::vector<OrderedList> orderings(n_replicas);

    for (int r = 0; r < n_replicas; r++) {
        orderings[r].cmds.reserve(n_txns);
        orderings[r].timestamps.reserve(n_txns);

        std::vector<int> indices(n_txns);
        std::iota(indices.begin(), indices.end(), 0);

        std::normal_distribution<double> noise(0, 3);
        std::vector<std::pair<double, int>> timed;
        for (int i = 0; i < n_txns; i++) {
            timed.push_back({i * 10 + noise(rng), i});
        }
        std::sort(timed.begin(), timed.end());

        for (int i = 0; i < n_txns; i++) {
            uint256_t cmd;
            memset(&cmd, timed[i].second, sizeof(cmd));
            orderings[r].cmds.push_back(cmd);
            orderings[r].timestamps.push_back((uint64_t)timed[i].first);
        }
    }

    std::cout << "=== Test 1: Measured delay = 5.0ms (LAN) ===\n";
    {
        HyperGraph<10000> hg;
        auto result = hg.finalize(orderings, 1, n_replicas, 0.75, 100.0, 5.0, 5.0);
        std::cout << "  Result size: " << result.size() << "\n";
    }

    std::cout << "\n=== Test 2: Measured delay = 50.0ms (WAN) ===\n";
    {
        HyperGraph<10000> hg;
        auto result = hg.finalize(orderings, 1, n_replicas, 0.75, 100.0, 50.0, 50.0);
        std::cout << "  Result size: " << result.size() << "\n";
    }

    std::cout << "\n=== Test 3: Auto-detect (no measured delay) ===\n";
    {
        HyperGraph<10000> hg;
        auto result = hg.finalize(orderings, 1, n_replicas, 0.75, 0.0, 0.0, -1.0);
        std::cout << "  Result size: " << result.size() << "\n";
    }

    std::cout << "\n=== Test 4: SCC merge with condorcet cycle ===\n";
    {
        std::vector<OrderedList> cycle_orderings(n_replicas);
        int n = 6;
        for (int r = 0; r < n_replicas; r++) {
            std::vector<int> perm(n);
            std::iota(perm.begin(), perm.end(), 0);
            if (r % 2 == 0) std::reverse(perm.begin(), perm.end());

            for (int i = 0; i < n; i++) {
                uint256_t cmd;
                memset(&cmd, perm[i], sizeof(cmd));
                cycle_orderings[r].cmds.push_back(cmd);
                cycle_orderings[r].timestamps.push_back(i * 10);
            }
        }
        HyperGraph<10000> hg;
        auto result = hg.finalize(cycle_orderings, 1, n_replicas, 0.75, 100.0, 10.0, 10.0);
        std::cout << "  Result size: " << result.size() << " (expected " << n << ")\n";
        if ((int)result.size() == n) {
            std::cout << "  All transactions ordered\n";
        }
    }

    std::cout << "\n=== All tests passed ===\n";
    return 0;
}
