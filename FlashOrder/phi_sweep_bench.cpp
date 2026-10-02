/**
 * phi_sweep_bench.cpp - Φ sensitivity benchmark for FlashOrder-D
 *
 * Expanded sweep: Φ × delay × block_size
 * Reports: per-phase timing, HyperEdge cluster count, delta value, SCC stats
 */

#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <algorithm>
#include <chrono>
#include <numeric>
#include <fstream>
#include <string>
#include "hotstuff/entity.h"
#include "hotstuff/flashorder.h"

using namespace hotstuff;
using namespace FlashOrder;

static uint256_t make_txn_id(uint32_t txn_idx) {
    bytearray_t raw(32, 0);
    raw[0] = static_cast<uint8_t>(txn_idx & 0xff);
    raw[1] = static_cast<uint8_t>((txn_idx >> 8) & 0xff);
    raw[2] = static_cast<uint8_t>((txn_idx >> 16) & 0xff);
    raw[3] = static_cast<uint8_t>((txn_idx >> 24) & 0xff);
    return uint256_t(raw);
}

static std::vector<OrderedList>
generate_orderings(int n_replicas, int n_txns, double noise_std, uint32_t seed) {
    // noise_std is preserved as a seed variant: callers can still perturb
    // the shuffle stream via the parameter without changing determinism
    // for a given (seed, noise_std) pair.
    std::mt19937 rng(seed ^ static_cast<uint32_t>(noise_std * 1000.0));
    std::vector<OrderedList> orderings(n_replicas);
    for (int r = 0; r < n_replicas; r++) {
        orderings[r].cmds.reserve(n_txns);
        orderings[r].timestamps.reserve(n_txns);
        std::vector<int> indices(n_txns);
        std::iota(indices.begin(), indices.end(), 0);
        std::shuffle(indices.begin(), indices.end(), rng);
        for (int i = 0; i < n_txns; i++) {
            uint256_t cmd = make_txn_id(static_cast<uint32_t>(indices[i]));
            orderings[r].cmds.push_back(cmd);
            orderings[r].timestamps.push_back(static_cast<uint64_t>(i) * 100ULL);
        }
    }
    return orderings;
}

struct BenchResult {
    double phi;
    int n_txns, n_replicas;
    double delay_ms, gen_param;
    double prefs_ms, canon_ms, clusters_ms, hypergraph_ms, topo_ms, total_ms;
    int n_hyperedges, n_clusters, n_sccs, n_multi_sccs, max_scc_size;
    int delta;
    int repeats;
};

static BenchResult
run_single(double phi, int n_txns, int n_replicas, double delay_ms,
           double gen_param, double noise_std, int repeats, uint32_t base_seed) {
    BenchResult r{};
    r.phi = phi; r.n_txns = n_txns; r.n_replicas = n_replicas;
    r.delay_ms = delay_ms; r.gen_param = gen_param;

    double sp=0,sc=0,scl=0,sh=0,st=0,sall=0;
    int s_delta=0, s_hyperedges=0, s_sccs=0, s_multi=0, max_sz=0;
    for (int i = 0; i < repeats; i++) {
        auto ol = generate_orderings(n_replicas, n_txns, noise_std, base_seed + i*7);
        HyperGraph<10000> hg;
        hg.finalize(ol, n_replicas/3, n_replicas, 0.75, gen_param, delay_ms, delay_ms, phi);
        const auto &p = hg.get_profile();
        sp+=p.preferences_ms; sc+=p.canonical_ms; scl+=p.clusters_ms;
        sh+=p.hypergraph_ms; st+=p.topo_sort_ms; sall+=p.total_ms;
        s_delta+=p.delta;
        s_hyperedges+=p.n_hyperedges; s_sccs+=p.n_sccs; s_multi+=p.n_multi_sccs;
        max_sz=std::max(max_sz,p.max_scc_size);
    }
    int n=repeats;
    r.delta=s_delta/n;
    r.prefs_ms=sp/n; r.canon_ms=sc/n; r.clusters_ms=scl/n;
    r.hypergraph_ms=sh/n; r.topo_ms=st/n; r.total_ms=sall/n;
    r.n_hyperedges=s_hyperedges/n; r.n_clusters=r.n_hyperedges;
    r.n_sccs=s_sccs/n; r.n_multi_sccs=s_multi/n; r.max_scc_size=max_sz;
    r.repeats=repeats;
    return r;
}

int main(int argc, char** argv) {
    bool targeted = false;
    int block_size_override = 0;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--targeted") {
            targeted = true;
        } else if (arg == "--block-size") {
            block_size_override = 400;
            if (i + 1 < argc) {
                std::string next = argv[i + 1];
                if (!next.empty() && next[0] != '-') {
                    try {
                        int v = std::stoi(next);
                        if (v > 0) block_size_override = v;
                        ++i;
                    } catch (...) {
                    }
                }
            }
        }
    }

    double phis_a[]      = {0.0, 0.075, 0.15, 0.30, 0.5, 1.0, 2.0, 5.0, 10.0, 100.0};
    int block_sizes_a[]  = {100, 800};
    double delays_a[]    = {5.0, 50.0, 100.0, 200.0};

    double phis_t[]      = {2.0, 4.0, 6.0};
    int block_sizes_t[]  = {100, 400, 800};
    double delays_t[]    = {50.0};

    const double* phis       = phis_a;
    const int*    block_sizes = block_sizes_a;
    const double* delays      = delays_a;
    size_t n_phi   = sizeof(phis_a) / sizeof(phis_a[0]);
    size_t n_block = sizeof(block_sizes_a) / sizeof(block_sizes_a[0]);
    size_t n_delay = sizeof(delays_a) / sizeof(delays_a[0]);

    if (targeted) {
        phis       = phis_t;
        block_sizes = block_sizes_t;
        delays      = delays_t;
        n_phi   = sizeof(phis_t) / sizeof(phis_t[0]);
        n_block = sizeof(block_sizes_t) / sizeof(block_sizes_t[0]);
        n_delay = sizeof(delays_t) / sizeof(delays_t[0]);

        if (block_size_override > 0) {
            block_sizes = &block_size_override;
            n_block = 1;
        }
    }

    int n_rep          = 21;
    int repeats        = 10;
    double noise_std   = 3.0;
    double gen_param   = 100.0;

    std::ostream* out = &std::cout;
    std::ofstream fout;
    if (targeted) {
        fout.open("phi_sweep_targeted.csv");
        out = &fout;
    }

    // CSV header
    *out << "phi,n_txns,n_replicas,delay_ms,delta,n_hyperedges,n_clusters,n_sccs,"
         << "preferences_ms,canonical_ms,clusters_ms,hypergraph_ms,topo_sort_ms,total_ms,"
         << "scc_multi,max_scc_size\n";
    *out << std::fixed << std::setprecision(3);

    for (size_t bi = 0; bi < n_block; bi++) {
        int b = block_sizes[bi];
        for (size_t di = 0; di < n_delay; di++) {
            double delay = delays[di];
            for (size_t pi = 0; pi < n_phi; pi++) {
                double phi = phis[pi];
                auto r = run_single(phi, b, n_rep, delay, gen_param,
                                    noise_std, repeats, 42);
                *out << r.phi << ","
                     << r.n_txns << ","
                     << r.n_replicas << ","
                     << (int)r.delay_ms << ","
                     << r.delta << ","
                     << r.n_hyperedges << ","
                     << r.n_clusters << ","
                     << r.n_sccs << ","
                     << r.prefs_ms << ","
                     << r.canon_ms << ","
                     << r.clusters_ms << ","
                     << r.hypergraph_ms << ","
                     << r.topo_ms << ","
                     << r.total_ms << ","
                     << r.n_multi_sccs << ","
                     << r.max_scc_size << "\n";
            }
        }
    }
    return 0;
}
