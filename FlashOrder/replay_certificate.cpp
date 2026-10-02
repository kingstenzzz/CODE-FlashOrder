// replay_certificate.cpp -- read a certificate (rows of permutations) from a
// file, run the deployed HyperGraph<10000>::finalize (the exact path used by
// flashorder_propose), and print the final order + SCC/cluster telemetry for
// bit-exact cross-check against the Python port HG_PREFERENCE_DISAGREEMENT.py.
//
// Usage: replay_certificate <rows_file> <n_nodes> <f> <gamma> <k_d>
//   rows_file: <n_nodes> lines, each a permutation of 0..m-1 (space-separated).
//   margin  = FLASHORDER_P_MARGIN (default 1.2)
//   strict_repair = FLASHORDER_P_STRICT_REPAIR (default off)
//
// Output (stdout):
//   ORDER:        txn indices in final order
//   DELTA:        logical delta
//   N_CLUSTERS:   number of delta clusters (hyperedges)
//   N_SCCS:       number of quotient SCCs
//   N_MULTI_SCCS: number of multi-node SCCs
//   MAX_SCC_SIZE: max cluster count in an SCC (not txn count)
//   GROUPS:       final group sizes (txn counts), in output order

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include "hotstuff/entity.h"
#include "hotstuff/flashorder.h"

using namespace hotstuff;
using namespace FlashOrder;

static uint256_t make_txn_id(uint32_t i) {
    bytearray_t raw(32, 0);
    raw[0] = i & 0xff;
    raw[1] = (i >> 8) & 0xff;
    raw[2] = (i >> 16) & 0xff;
    raw[3] = (i >> 24) & 0xff;
    return uint256_t(raw);
}

static uint32_t txn_index(const uint256_t &x) {
    bytearray_t ba = x;
    return (uint32_t)ba[0] | ((uint32_t)ba[1] << 8) |
           ((uint32_t)ba[2] << 16) | ((uint32_t)ba[3] << 24);
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s <rows_file> <n_nodes> <f> <gamma> <k_d>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    int n = atoi(argv[2]);
    int f = atoi(argv[3]);
    double gamma = atof(argv[4]);
    double kd = atof(argv[5]);

    std::ifstream in(path);
    if (!in) { fprintf(stderr, "cannot open %s\n", path); return 2; }

    std::vector<OrderedList> lists;
    std::string line;
    int m = -1;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::vector<int> row;
        int v;
        while (ss >> v) row.push_back(v);
        if (m < 0) m = (int)row.size();
        if ((int)row.size() != m) {
            fprintf(stderr, "row length mismatch: %d vs %d\n", (int)row.size(), m);
            return 2;
        }
        OrderedList ol;
        ol.cmds.reserve(m);
        ol.timestamps.reserve(m);
        for (int i = 0; i < m; i++) {
            ol.cmds.push_back(make_txn_id((uint32_t)row[i]));
            // ascending timestamps => compute_preferences() will NOT re-sort,
            // so cmds order equals the certificate row order.
            ol.timestamps.push_back((uint64_t)i * 100ULL);
        }
        lists.push_back(std::move(ol));
    }
    if ((int)lists.size() != n) {
        fprintf(stderr, "expected %d rows, got %zu\n", n, lists.size());
        return 2;
    }

    HyperGraph<10000> hg;
    std::vector<uint256_t> fo = hg.finalize(lists, f, n, gamma,
                                            100.0, 100.0, -1.0, kd);
    const auto &p = hg.get_profile();

    printf("ORDER:");
    for (size_t i = 0; i < fo.size(); i++)
        printf(" %u", txn_index(fo[i]));
    printf("\n");
    printf("DELTA: %d\n", p.delta);
    printf("N_CLUSTERS: %d\n", p.n_hyperedges);
    printf("N_SCCS: %d\n", p.n_sccs);
    printf("N_MULTI_SCCS: %d\n", p.n_multi_sccs);
    printf("MAX_SCC_SIZE: %d\n", p.max_scc_size);
    printf("GROUPS:");
    for (int g : p.final_group_sizes) printf(" %d", g);
    printf("\n");
    return 0;
}
