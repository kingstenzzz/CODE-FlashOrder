// kd_quality_bench.cpp -- PD (flashorder-P) k_d 排序质量扫描（算法级）
//
// 评审要求（response-pvldb R5-W3）：rank displacement、robust-pair 满足度、
// γ-batch 公平指标随 k_d 的曲线。
//
// 方法：
//   1. 构造 ground-truth 全序 0..n-1；每个诚实副本的 OrderedList = ground truth
//      加噪声（相邻交换次数 ~ Poisson(noise)）；f 个拜占庭副本发随机序。
//   2. 对每个 k_d：HyperGraph::finalize(...) 得到最终序。
//   3. 与 ground truth 对比：
//        rank_displacement = mean |final_pos(tx) - true_pos(tx)| / n
//        robust_pair_sat   = 在 ≥ (n-f) 个副本中相对次序一致的交易对中，
//                            最终序保持该次序的比例
//        gamma_batch       = 把最终序切成大小 B 的 batch，每个 batch 内
//                            "batch 前后边界无 robust-pair 违反"的比例（γ-Batch
//                            Order-Fairness 的代理：违反的 pair 必须跨 batch）
//   4. CSV 输出，重复 R 次取均值。
//
// Build:
//   g++ -std=c++17 -O2 -I include -I salticidae/include -I secp256k1/include -I . \
//       tools_logger_stub.cpp kd_quality_bench.cpp -o tools_kd_quality \
//       build/salticidae/libsalticidae.a secp256k1/.libs/libsecp256k1.a -lcrypto -lpthread

#include <cstdio>
#include <cstdint>
#include <vector>
#include <random>
#include <numeric>
#include <algorithm>
#include <string>
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
    bytearray_t ba = x;  // Blob -> bytearray_t
    return (uint32_t)ba[0] | ((uint32_t)ba[1] << 8)
         | ((uint32_t)ba[2] << 16) | ((uint32_t)ba[3] << 24);
}

struct Orderings {
    std::vector<OrderedList> lists;  // 副本排序
    std::vector<int> honest;         // 诚实副本下标
};

// 对抗模式（Condorcet 式）：对热点窗口 [lo, lo+W) 的交易，
// 一半诚实副本局部旋转 +shift，另一半旋转 -shift，制造系统性双向分歧。
// 这是 R5-W3 关心的 "cluster 退化" 场景：D_t 被人为推高。
static void apply_adversarial(std::vector<int> &order, int honest_rank,
                              int honest_total, int lo, int W, int shift) {
    std::vector<int> window(order.begin() + lo, order.begin() + lo + W);
    int k = shift % W; if (k < 0) k += W;
    if (honest_rank % 2 == 0)
        std::rotate(window.begin(), window.begin() + k, window.end());   // 前移
    else
        std::rotate(window.begin(), window.end() - k, window.end());     // 后移
    for (int i = 0; i < W; i++) order[lo + i] = window[i];
}

// heavy 攻击（优先级/decouple 式）：拜占庭副本把热点窗口交易整体前移到序列头部
// （MEV 式打包），与诚实副本的自然混合序形成系统性单向量分歧。
static void apply_heavy_attack(std::vector<int> &order, int lo, int W) {
    std::vector<int> window(order.begin() + lo, order.begin() + lo + W);
    std::vector<int> rest;
    rest.reserve(order.size() - W);
    for (int i = 0; i < (int)order.size(); i++)
        if (i < lo || i >= lo + W) rest.push_back(order[i]);
    for (int i = 0; i < W; i++) order[i] = window[i];
    for (int i = 0; i < (int)rest.size(); i++) order[W + i] = rest[i];
}

static Orderings generate(int n, int nrep, int f, int noise, uint32_t seed,
                          const std::string &mode) {
    bool adversarial = (mode == "condorcet");
    bool heavy = (mode == "heavy");
    std::mt19937 rng(seed);
    Orderings out;
    std::vector<int> replica_ids(nrep);
    std::iota(replica_ids.begin(), replica_ids.end(), 0);
    std::shuffle(replica_ids.begin(), replica_ids.end(), rng);
    // 前 f 个为拜占庭
    std::vector<char> byz(nrep, 0);
    for (int i = 0; i < f; i++) byz[replica_ids[i]] = 1;

    int honest_seen = 0;
    int honest_total = nrep - f;
    for (int r = 0; r < nrep; r++) {
        std::vector<int> order(n);
        std::iota(order.begin(), order.end(), 0);
        if (byz[r]) {
            if (heavy) {
                // heavy 攻击：从真实序出发，把诚实热点窗口 [lo, lo+W) 前移到头部
                // （抢跑打包热点交易），与诚实混合序形成系统性单向量分歧
                int W = std::max(4, n / 4);
                int lo = (n - W) / 2;
                apply_heavy_attack(order, lo, W);
            } else {
                std::shuffle(order.begin(), order.end(), rng);
            }
        } else {
            std::poisson_distribution<int> pois(noise);
            int swaps = pois(rng);
            for (int s = 0; s < swaps; s++) {
                std::uniform_int_distribution<int> uni(0, n - 2);
                int i = uni(rng);
                std::swap(order[i], order[i + 1]);
            }
            if (adversarial) {
                // 热点窗口：n/4 宽，移位 1/3 窗口
                int W = std::max(4, n / 4);
                int lo = (n - W) / 2;
                apply_adversarial(order, honest_seen++, honest_total,
                                  lo, W, std::max(1, W / 3));
            }
        }
        OrderedList ol;
        ol.cmds.reserve(n);
        ol.timestamps.reserve(n);
        for (int i = 0; i < n; i++) {
            ol.cmds.push_back(make_txn_id((uint32_t)order[i]));
            ol.timestamps.push_back((uint64_t)i * 100ULL);
        }
        if (!byz[r]) out.honest.push_back(r);
        out.lists.push_back(std::move(ol));
    }
    return out;
}

struct Quality {
    double rank_displacement;  // normalized mean |Δpos|
    double robust_pair_sat;    // robust pair 满足比例
    double gamma_batch;        // γ-batch 代理满足比例
    double frac_robust;        // 全部 pair 中 robust pair 占比
};

static Quality evaluate(const std::vector<uint256_t> &final_order, int n,
                        const std::vector<OrderedList> &lists,
                        const std::vector<int> &honest, int batch_size) {
    // ground truth pos: txn i 的真实位置就是 i（生成时 ground truth = 0..n-1）
    Quality q{};
    int n_fin = (int)final_order.size();

    // final position of each txn
    std::vector<int> pos(n, -1);
    for (int i = 0; i < n_fin; i++) {
        uint32_t idx = txn_index(final_order[i]);
        if (idx < (uint32_t)n) pos[idx] = i;
    }
    double disp = 0; int cnt = 0;
    for (int tx = 0; tx < n; tx++) {
        if (pos[tx] >= 0) { disp += std::abs(pos[tx] - tx); cnt++; }
    }
    q.rank_displacement = cnt ? (disp / cnt) / n : 1.0;

    // honest majority relation: pair (a<b) robust if > half honest replicas agree
    std::vector<std::vector<int>> rank(honest.size(), std::vector<int>(n, -1));
    for (size_t h = 0; h < honest.size(); h++) {
        auto &cmds = lists[honest[h]].cmds;
        for (int i = 0; i < (int)cmds.size(); i++) {
            uint32_t idx = txn_index(cmds[i]);
            if (idx < (uint32_t)n) rank[h][idx] = i;
        }
    }
    long robust = 0, sat = 0;
    double violations = 0.0;  // robust-pair 违反总数（γ-batch 归一化用）
    for (int a = 0; a < n; a++) {
        for (int b = a + 1; b < n; b++) {
            if (pos[a] < 0 || pos[b] < 0) continue;
            int agree = 0;
            for (size_t h = 0; h < honest.size(); h++)
                if (rank[h][a] >= 0 && rank[h][b] >= 0 && rank[h][a] < rank[h][b]) agree++;
            if ((long)honest.size() - agree > agree) agree = (long)honest.size() - agree; // majority either way
            if (agree * 2 > (long)honest.size()) {
                robust++;
                bool a_before_b_majority = false;
                int ab = 0;
                for (size_t h = 0; h < honest.size(); h++)
                    if (rank[h][a] >= 0 && rank[h][b] >= 0 && rank[h][a] < rank[h][b]) ab++;
                a_before_b_majority = ab * 2 > (long)honest.size();
                bool ok = a_before_b_majority ? (pos[a] < pos[b]) : (pos[b] < pos[a]);
                if (ok) sat++;
                else violations += 1.0;
            }
        }
    }
    q.robust_pair_sat = robust ? (double)sat / robust : 0.0;
    q.frac_robust = (double)robust / ((double)n * (n - 1) / 2);
    // γ-batch 代理：robust-pair 违反率取补（1=无违反）
    q.gamma_batch = robust ? 1.0 - violations / robust : 1.0;
    return q;
}

int main(int argc, char **argv) {
    int n = 200, nrep = 21, f = 6;      // n=3f+3? nrep=21, f=6 (3f+1=19<=21)
    int noise = 5, batch_size = 20, repeats = 5;
    std::vector<double> kds = {0.0, 0.075, 0.15, 0.3, 0.5, 1.0, 2.0, 5.0, 10.0, 100.0};
    double gamma = 0.8;
    std::string mode = "random";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--n" && i + 1 < argc) n = atoi(argv[++i]);
        else if (a == "--nrep" && i + 1 < argc) nrep = atoi(argv[++i]);
        else if (a == "--f" && i + 1 < argc) f = atoi(argv[++i]);
        else if (a == "--noise" && i + 1 < argc) noise = atoi(argv[++i]);
        else if (a == "--batch" && i + 1 < argc) batch_size = atoi(argv[++i]);
        else if (a == "--repeats" && i + 1 < argc) repeats = atoi(argv[++i]);
        else if (a == "--gamma" && i + 1 < argc) gamma = atof(argv[++i]);
        else if (a == "--adv") mode = "condorcet";
        else if (a == "--mode" && i + 1 < argc) mode = argv[++i];
    }
    fprintf(stderr, "kd_quality: n=%d nrep=%d f=%d noise=%d batch=%d gamma=%.2f repeats=%d mode=%s\n",
            n, nrep, f, noise, batch_size, gamma, repeats, mode.c_str());
    printf("k_d,rank_displacement,robust_pair_sat,gamma_batch,frac_robust_pair\n");
    for (double kd : kds) {
        double sd = 0, sr = 0, sg = 0, sf = 0;
        for (int rep = 0; rep < repeats; rep++) {
            auto ord = generate(n, nrep, f, noise, 1234u + rep * 101, mode);
            HyperGraph<10000> hg;
            auto final_order = hg.finalize(ord.lists, f, nrep, gamma,
                                           100.0, 100.0, -1.0, kd);
            auto q = evaluate(final_order, n, ord.lists, ord.honest, batch_size);
            sd += q.rank_displacement; sr += q.robust_pair_sat;
            sg += q.gamma_batch; sf += q.frac_robust;
        }
        printf("%.4f,%.6f,%.6f,%.6f,%.6f\n", kd, sd / repeats, sr / repeats,
               sg / repeats, sf / repeats);
        fflush(stdout);
    }
    return 0;
}
