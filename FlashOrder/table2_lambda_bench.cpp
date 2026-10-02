// table2_lambda_bench.cpp -- Table 2 methodology lambda (margin) sensitivity
// on the flashorder-P C++ ordering core (HyperGraph::finalize).
//
// Load & metrics replicate simulations/table2_simulation.py verbatim:
//   - honest receive times: send_t = cumsum(Exp(gen=10ms)) + Exp(net=100ms)
//   - f=2 byzantine replicas (nodes 0..f-1) REVERSE their entire local order
//   - 30 inducing txs appended: byz see them early (100 + i*0.5),
//     honest see them at the very end (100000 + num_txs + i)
//   - robust pair: |2*P_ij - n| > floor(n(1-gamma)) + f + 1 on the 100 orig txs
//   - MaxDisp vs honest-median receive-time rank (absolute positions)
// Sweep: FLASHORDER_P_MARGIN over lambda in {1.0..2.0}, gamma in
//   {0.80 (n=101), 0.51 (n=401), 1.00 (n=101)}, 20 trials.
#include <cstdio>
#include <cstdint>
#include <vector>
#include <random>
#include <numeric>
#include <algorithm>
#include <string>
#include <cstdlib>
#include <cmath>
#include "hotstuff/entity.h"
#include "hotstuff/flashorder.h"
using namespace hotstuff; using namespace FlashOrder;

static uint256_t make_txn_id(uint32_t i){bytearray_t raw(32,0);raw[0]=i&0xff;raw[1]=(i>>8)&0xff;raw[2]=(i>>16)&0xff;raw[3]=(i>>24)&0xff;return uint256_t(raw);}
static uint32_t txn_index(const uint256_t &x){bytearray_t ba=x;return (uint32_t)ba[0]|((uint32_t)ba[1]<<8)|((uint32_t)ba[2]<<16)|((uint32_t)ba[3]<<24);}

static constexpr int NUM_TXS = 100;
static constexpr int N_INDUCING = 30;

struct Trial {
    std::vector<OrderedList> lists;   // nrep lists over 130 txs (ts ascending)
    std::vector<std::vector<int>> ranks; // per-replica rank of each of 130 txs
    std::vector<double> honest_median_ts; // for the 100 orig txs
};

// ts in "ms*1000" micro-units to keep integer precision
static Trial gen_trial(int nrep, int f, double gen_p, double net_p, uint32_t seed) {
    std::mt19937 rng(seed);
    std::exponential_distribution<double> g(gen_p), d(net_p);
    int total = NUM_TXS + N_INDUCING;
    // receive times per replica, double ms
    std::vector<std::vector<double>> t(nrep, std::vector<double>(total));
    std::vector<double> send(NUM_TXS);
    { double acc = 0; for (int i = 0; i < NUM_TXS; i++) { acc += g(rng); send[i] = acc; } }
    for (int r = 0; r < nrep; r++) {
        for (int i = 0; i < NUM_TXS; i++) t[r][i] = send[i] + d(rng);
        bool byz = r < f;
        for (int i = 0; i < N_INDUCING; i++)
            t[r][NUM_TXS + i] = byz ? (100.0 + i * 0.5) : (100000.0 + NUM_TXS + i);
        if (byz) std::reverse(t[r].begin(), t[r].end());  // reverse full local order
    }
    Trial tr;
    // honest median ts of orig txs (python: median over rows f.. of orig cols)
    {
        std::vector<std::vector<double>> cols(NUM_TXS, std::vector<double>(nrep - f));
        for (int r = f; r < nrep; r++)
            for (int i = 0; i < NUM_TXS; i++) cols[i][r - f] = t[r][i];
        tr.honest_median_ts.resize(NUM_TXS);
        for (int i = 0; i < NUM_TXS; i++) {
            auto v = cols[i]; std::sort(v.begin(), v.end());
            tr.honest_median_ts[i] = v[v.size() / 2];
        }
    }
    for (int r = 0; r < nrep; r++) {
        std::vector<int> idx(total); std::iota(idx.begin(), idx.end(), 0);
        std::stable_sort(idx.begin(), idx.end(),
                         [&](int a, int b){ return t[r][a] < t[r][b]; });
        OrderedList ol; ol.cmds.reserve(total); ol.timestamps.reserve(total);
        std::vector<int> rank(total);
        for (int p = 0; p < total; p++) {
            ol.cmds.push_back(make_txn_id((uint32_t)idx[p]));
            ol.timestamps.push_back((uint64_t)std::llround(t[r][idx[p]] * 1000.0));
            rank[idx[p]] = p;
        }
        tr.lists.push_back(std::move(ol));
        tr.ranks.push_back(std::move(rank));
    }
    return tr;
}

int main(int argc, char **argv) {
    int f = 2, trials = 20;
    double kd = 1.0, gen_p = 10.0, net_p = 100.0;
    std::vector<double> lambdas = {1.0, 1.05, 1.1, 1.2, 1.5, 2.0};
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--f") f = atoi(argv[++i]);
        else if (a == "--trials") trials = atoi(argv[++i]);
        else if (a == "--kd") kd = atof(argv[++i]);
        else if (a == "--gen") gen_p = atof(argv[++i]);
        else if (a == "--net") net_p = atof(argv[++i]);
        else if (a == "--margin") setenv("FLASHORDER_P_MARGIN", argv[++i], 1);
        else if (a == "--strict") setenv("FLASHORDER_P_STRICT_REPAIR", "1", 1);
        else if (a == "--lambdas") { lambdas.clear(); char *tk = strtok(argv[++i], ","); while (tk) { lambdas.push_back(atof(tk)); tk = strtok(nullptr, ","); } }
        else if (a == "--instrument") setenv("T2L_INSTRUMENT", "1", 1);
    }
    bool instr = getenv("T2L_INSTRUMENT") != nullptr;
    const char *ml = getenv("FLASHORDER_P_MARGIN");
    fprintf(stderr, "table2-lambda: f=%d trials=%d kd=%.3f gen=%.1f net=%.1f margin=%s strict=%s\n",
            f, trials, kd, gen_p, net_p, ml ? ml : "1.2",
            getenv("FLASHORDER_P_STRICT_REPAIR") ? "on" : "off");
    printf("gamma,n,lambda,rps_mean,rps_std,maxdisp_mean,maxdisp_max\n");
    struct Cfg { double gamma; int n; };
    Cfg cfgs[] = {{0.80, 101}, {0.51, 401}, {1.00, 101}};
    for (auto &cfg : cfgs) {
        // trials are lambda-independent: generate once, reuse across lambda
        std::vector<Trial> ts;
        for (int s = 0; s < trials; s++) ts.push_back(gen_trial(cfg.n, f, gen_p, net_p, 7717u + s));
        for (double lam : lambdas) {
            char buf[16]; snprintf(buf, sizeof buf, "%.6g", lam); setenv("FLASHORDER_P_MARGIN", buf, 1);
            double rps_sum = 0, rps_sq = 0; long disp_sum = 0; long disp_max = 0;
            for (int s = 0; s < trials; s++) {
                const Trial &tr = ts[s];
                HyperGraph<10000> hg;
                auto fo = hg.finalize(const_cast<std::vector<OrderedList>&>(tr.lists),
                                      f, cfg.n, cfg.gamma, 100.0, 100.0, -1.0, kd);
                int n = cfg.n;
                std::vector<int> pos(NUM_TXS, NUM_TXS + N_INDUCING);
                for (size_t p = 0; p < fo.size(); p++) {
                    uint32_t id = txn_index(fo[p]);
                    if (id < (uint32_t)NUM_TXS && pos[id] > (int)p) pos[id] = (int)p;
                }
                // preference over full 130-tx orderings, orig pairs only
                long total_pairs = 0, correct = 0;
                int threshold = std::max(1, (int)(std::floor(n * (1.0 - cfg.gamma))) + f + 1);
                std::vector<int> cnt(NUM_TXS * NUM_TXS, 0);
                for (int r = 0; r < n; r++)
                    for (int i = 0; i < NUM_TXS; i++)
                        for (int j = i + 1; j < NUM_TXS; j++)
                            if (tr.ranks[r][i] < tr.ranks[r][j]) cnt[i * NUM_TXS + j]++;
                for (int i = 0; i < NUM_TXS; i++)
                    for (int j = i + 1; j < NUM_TXS; j++) {
                        int pij = cnt[i * NUM_TXS + j];
                        long P_ij = 2L * pij - n;
                        if (std::labs(P_ij) > threshold) {
                            total_pairs++;
                            bool exp_i = P_ij > 0;
                            bool act_i = pos[i] < pos[j];
                            if (exp_i == act_i) correct++;
                        }
                    }
                double rps = total_pairs ? (double)correct / total_pairs : 1.0;
                rps_sum += rps; rps_sq += rps * rps;
                // MaxDisp vs honest-median rank (absolute positions, python口径)
                std::vector<int> hidx(NUM_TXS); std::iota(hidx.begin(), hidx.end(), 0);
                std::sort(hidx.begin(), hidx.end(), [&](int a, int b){
                    return tr.honest_median_ts[a] < tr.honest_median_ts[b]; });
                std::vector<int> hrank(NUM_TXS);
                for (int p = 0; p < NUM_TXS; p++) hrank[hidx[p]] = p;
                long md = 0;
                for (int i = 0; i < NUM_TXS; i++)
                    md = std::max(md, std::labs((long)pos[i] - hrank[i]));
                disp_sum += md; disp_max = std::max(disp_max, md);
            }
            double mean = rps_sum / trials;
            double sd = std::sqrt(std::max(0.0, rps_sq / trials - mean * mean));
            printf("%.2f,%d,%.6g,%.4f,%.4f,%.1f,%ld\n", cfg.gamma, cfg.n, lam,
                   mean, sd, (double)disp_sum / trials, disp_max);
            fflush(stdout);
        }
        if (instr) {
            // flashorder-P 自身聚类 + pref 矩阵的簇间比值分布 (trial 0)
            const Trial &tr = ts[0];
            HyperGraph<10000> hg;
            hg.finalize(const_cast<std::vector<OrderedList>&>(tr.lists),
                        f, cfg.n, cfg.gamma, 100.0, 100.0, -1.0, kd);
            int H = (int)hg.get_hyperedges().size();
            std::vector<double> ratios; int bidi = 0, pairs = 0;
            for (int i = 0; i < H; i++)
                for (int j = i + 1; j < H; j++) {
                    long pij = 0, pji = 0;
                    for (int a : hg.get_hyperedges()[i].members)
                        for (int b : hg.get_hyperedges()[j].members) {
                            pij += hg.pref_at(a, b);
                            pji += hg.pref_at(b, a);
                        }
                    pairs++;
                    if (pij == 0 || pji == 0) continue;
                    bidi++;
                    ratios.push_back((double)std::max(pij, pji) / std::min(pij, pji));
                }
            std::sort(ratios.begin(), ratios.end());
            fprintf(stderr, "[instr] gamma=%.2f n=%d clusters=%d pairs=%d bidi=%d "
                    "ratio_min=%.1f ratio_med=%.1f ratio_max=%.1f\n",
                    cfg.gamma, cfg.n, H, pairs, bidi,
                    ratios.empty() ? 0.0 : ratios.front(),
                    ratios.empty() ? 0.0 : ratios[ratios.size() / 2],
                    ratios.empty() ? 0.0 : ratios.back());
            for (double pl : {1.0, 1.2, 2.0, 5.0, 10.0, 30.0, 50.0, 100.0}) {
                int kept = 0;
                for (double r : ratios) if (r > pl) kept++;
                fprintf(stderr, "[instr]   lambda=%.1f -> bidi edges kept=%d/%d\n",
                        pl, kept, (int)ratios.size());
            }
        }
    }
    return 0;
}
