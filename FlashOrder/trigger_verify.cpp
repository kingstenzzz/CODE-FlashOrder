// trigger_verify.cpp -- verify strict-repair trigger on the real HyperGraph core.
// Reads workload from stdin: "n_rows m f gamma" then n_rows lines of m tx ids.
// Usage: FLASHORDER_P_STRICT_REPAIR=1 ./trigger_verify < workload.txt
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <numeric>
#include <algorithm>
#include "hotstuff/entity.h"
#include "hotstuff/flashorder.h"

using namespace hotstuff;
using namespace FlashOrder;

static uint256_t make_txn_id(uint32_t i) {
    bytearray_t raw(32, 0);
    raw[0] = i & 0xff; raw[1] = (i >> 8) & 0xff;
    raw[2] = (i >> 16) & 0xff; raw[3] = (i >> 24) & 0xff;
    return uint256_t(raw);
}
static uint32_t txn_index(const uint256_t &x) {
    bytearray_t ba = x;
    return (uint32_t)ba[0] | ((uint32_t)ba[1] << 8) |
           ((uint32_t)ba[2] << 16) | ((uint32_t)ba[3] << 24);
}

int main(int argc, char **argv) {
    int n_rows, m, f; double gamma;
    if (scanf("%d %d %d %lf", &n_rows, &m, &f, &gamma) != 4) {
        fprintf(stderr, "usage: echo 'n m f gamma' + rows | trigger_verify [strict]\n");
        return 2;
    }
    int strict_on = (argc > 1) ? 1 : 0;
    if (strict_on) setenv("FLASHORDER_P_STRICT_REPAIR", "1", 1);
    else unsetenv("FLASHORDER_P_STRICT_REPAIR");

    std::vector<OrderedList> lists;
    for (int r = 0; r < n_rows; r++) {
        OrderedList ol;
        for (int i = 0; i < m; i++) {
            int x; if (scanf("%d", &x) != 1) return 2;
            ol.cmds.push_back(make_txn_id((uint32_t)x));
            ol.timestamps.push_back((uint64_t)i * 100ULL);
        }
        lists.push_back(std::move(ol));
    }

    HyperGraph<1000> hg;
    auto fo = hg.finalize(lists, f, n_rows, gamma);
    auto &pr = hg.get_profile();
    int thr = std::max(1, (int)(n_rows * (1.0 - gamma)) + f + 1);
    printf("strict=%d n=%d m=%d gamma=%.1f f=%d thr=%d delta=%d\n",
           strict_on, n_rows, m, gamma, f, thr, pr.delta);
    printf("repair_rounds=%d repair_merges=%d n_hyperedges=%d\n",
           pr.repair_rounds, pr.repair_merges, pr.n_hyperedges);
    printf("final order (%zu):", fo.size());
    for (auto &t : fo) printf(" %u", txn_index(t));
    printf("\n");
    return 0;
}
