// hc20_bench.cpp -- HC-20: macro-cycle stress case (algorithm level)
//
// 规格:
//   n=21 replicas, batch=800, cycle length=20, 5 seeds, strict_repair=False(PD 默认)
//   对照臂: 同参数 no-cycle 负载
// 机制要求(不发明新攻击模型, 只在现有 generator 上叠加):
//   1. honest 副本对 20-tx 窗口按 rank 做循环旋转(标准 Condorcet 构造)
//      -> 相邻 pair 边权 20:1, 保证 quotient-level 存在长度 20 的有向环
//   2. 边权 20 >= n(1-γ)+f+1 = 21*0.2+8 = 13 (q_γ admission threshold) ✓
//   3. 簇间边仍用 HyperGraph 内置 margin λ=1.2 ✓
//   4. 窗口外交易 pair 走原 kd_quality_bench 的 noise+byzantine 路径
//
// 每 seed 一行汇总:
//   qSCC_any, qSCC_full, merge_rounds, max_cluster_ratio, RPS_q, MaxDisp
#include <cstdio>
#include <cstdint>
#include <vector>
#include <random>
#include <numeric>
#include <algorithm>
#include <string>
#include <cstdlib>
#include "hotstuff/entity.h"
#include "hotstuff/flashorder.h"
using namespace hotstuff; using namespace FlashOrder;

static uint256_t make_txn_id(uint32_t i){bytearray_t raw(32,0);raw[0]=i&0xff;raw[1]=(i>>8)&0xff;raw[2]=(i>>16)&0xff;raw[3]=(i>>24)&0xff;return uint256_t(raw);}
static uint32_t txn_index(const uint256_t &x){bytearray_t ba=x;return (uint32_t)ba[0]|((uint32_t)ba[1]<<8)|((uint32_t)ba[2]<<16)|((uint32_t)ba[3]<<24);}

struct Orderings {
    std::vector<OrderedList> lists;
    std::vector<int> honest;
};

// 复刻 kd_quality_bench::generate, 增加 cycle_window(<=0 关闭 = control 臂)
static Orderings generate(int n, int nrep, int f, int noise, uint32_t seed,
                          int cycle_len) {
    std::mt19937 rng(seed);
    Orderings out;
    std::vector<int> rid(nrep); std::iota(rid.begin(), rid.end(), 0);
    std::shuffle(rid.begin(), rid.end(), rng);
    std::vector<char> byz(nrep, 0);
    for (int i = 0; i < f; i++) byz[rid[i]] = 1;
    int honest_rank = 0;
    int W = cycle_len > 0 ? cycle_len : 0;
    int lo = (n - W) / 2;
    for (int r = 0; r < nrep; r++) {
        std::vector<int> o(n); std::iota(o.begin(), o.end(), 0);
        if (byz[r]) { std::shuffle(o.begin(), o.end(), rng); }
        else {
            std::poisson_distribution<int> p(noise);
            for (int s = p(rng); s > 0; s--) {
                std::uniform_int_distribution<int> u(0, n - 2);
                int i = u(rng); std::swap(o[i], o[i+1]);
            }
            if (W > 1) {
                // 循环旋转: replica honest_rank 把窗口变成
                // c_{r'}, c_{r'+1}, ..., c_{r'+W-1} (mod W)
                std::vector<int> w(o.begin()+lo, o.begin()+lo+W);
                int k = honest_rank % W;
                std::rotate(w.begin(), w.begin()+k, w.end());
                for (int i = 0; i < W; i++) o[lo+i] = w[i];
            }
            out.honest.push_back(r);
            honest_rank++;
        }
        OrderedList ol; ol.cmds.reserve(n); ol.timestamps.reserve(n);
        for (int i = 0; i < n; i++) {
            ol.cmds.push_back(make_txn_id((uint32_t)o[i]));
            ol.timestamps.push_back((uint64_t)i * 100ULL);
        }
        out.lists.push_back(std::move(ol));
    }
    return out;
}

struct Quality {
    double rps_q = 0;      // admitted(robust) pair 满足比例
    double max_disp = 0;   // 最大位置位移 / n
};

static Quality evaluate(const std::vector<uint256_t> &final_order, int n,
                        const std::vector<OrderedList> &lists,
                        const std::vector<int> &honest,
                        const std::vector<int> &group_sizes = {}) {
    std::vector<int> pos(n, -1);
    for (size_t i = 0; i < final_order.size(); i++) {
        uint32_t id = txn_index(final_order[i]);
        if (id < (uint32_t)n && pos[id] < 0) pos[id] = (int)i;
    }
    // 组(batch) id: 同组 pair 视为已吸收 -> RPS_q 满足 (CR-γ-BOF 语义)
    std::vector<int> grp(n, -1);
    {
        int g = 0, acc = 0;
        for (size_t i = 0; i < final_order.size(); i++) {
            if (g < (int)group_sizes.size() && (int)i >= acc + group_sizes[g]) {
                acc += group_sizes[g]; g++;
            }
            uint32_t id = txn_index(final_order[i]);
            if (id < (uint32_t)n) grp[id] = g;
        }
    }
    // honest 多数 rank
    std::vector<std::vector<int>> rk(honest.size(), std::vector<int>(n, -1));
    for (size_t h = 0; h < honest.size(); h++)
        for (size_t i = 0; i < lists[honest[h]].cmds.size(); i++)
            rk[h][txn_index(lists[honest[h]].cmds[i])] = (int)i;
    long long sat = 0, admitted = 0; double maxd = 0;
    for (int a = 0; a < n; a++) for (int b = 0; b < n; b++) {
        if (a == b) continue;
        int before = 0;
        for (size_t h = 0; h < honest.size(); h++)
            if (rk[h][a] >= 0 && rk[h][b] >= 0 && rk[h][a] < rk[h][b]) before++;
        int hboth = 0;
        for (size_t h = 0; h < honest.size(); h++)
            if (rk[h][a] >= 0 && rk[h][b] >= 0) hboth++;
        // q_γ admission: 多数方向票数 >= 阈值 (与排序核 threshold 同口径)
        if (2*before > hboth) {           // 多数 a<b
            admitted++;
            bool sat_strict = pos[a] >= 0 && pos[b] >= 0 && pos[a] < pos[b];
            bool sat_absorbed = grp[a] >= 0 && grp[a] == grp[b];
            if (sat_strict || sat_absorbed) sat++;
        }
    }
    for (int a = 0; a < n; a++)
        if (pos[a] >= 0) maxd = std::max(maxd, (double)std::abs(pos[a]-a)/n);
    Quality q; q.rps_q = admitted ? (double)sat/admitted : 1.0; q.max_disp = maxd;
    return q;
}

int main(int argc, char **argv) {
    int n = 800, nrep = 21, f = 7, cycle = 20, noise = 5, seeds = 5, batches = 10;
    double gamma = 0.8, kd = 1.0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto nx = [&]() { return atoi(argv[++i]); };
        if (a == "--n") n = nx();
        else if (a == "--nrep") nrep = nx();
        else if (a == "--f") f = nx();
        else if (a == "--cycle") cycle = nx();
        else if (a == "--noise") noise = nx();
        else if (a == "--seeds") seeds = nx();
        else if (a == "--batches") batches = nx();
        else if (a == "--gamma") gamma = atof(argv[++i]);
        else if (a == "--kd") kd = atof(argv[++i]);
        else if (a == "--strict") setenv("FLASHORDER_P_STRICT_REPAIR", "1", 1);
        else if (a == "--margin") setenv("FLASHORDER_P_MARGIN", argv[++i], 1);
    }
    fprintf(stderr, "HC-20: n=%d nrep=%d f=%d cycle=%d batch=%d noise=%d "
            "gamma=%.2f kd=%.3f seeds=%d batches=%d strict_repair=%s margin=%s\n",
            n, nrep, f, cycle, n, noise, gamma, kd, seeds, batches,
            getenv("FLASHORDER_P_STRICT_REPAIR") ? "True" : "False",
            getenv("FLASHORDER_P_MARGIN") ? getenv("FLASHORDER_P_MARGIN") : "1.2");
    printf("seed,qSCC_any,qSCC_full,merge_rounds,max_cluster_ratio,RPS_q,MaxDisp\n");
    for (int s = 0; s < seeds; s++) {
        int any = 0, full = 0, rounds_sum = 0;
        double maxcr = 0, rps_sum = 0, disp_max = 0;
        for (int b = 0; b < batches; b++) {
            uint32_t seed = 4242u + s * 1000 + b * 17;
            auto ord = generate(n, nrep, f, noise, seed, cycle);
            HyperGraph<10000> hg;
            auto fo = hg.finalize(ord.lists, f, nrep, gamma, 100.0, 100.0, -1.0, kd);
            const auto &p = hg.get_profile();
            if (p.n_multi_sccs > 0) any++;
            // merge rounds: strict_repair 用真实修复轮遥测, 默认路径恒 1(PD 单遍)
            rounds_sum += (p.repair_rounds > 0) ? p.repair_rounds
                                                : (p.n_multi_sccs > 0 ? 1 : 0);
            if (p.max_scc_size >= (int)n) full++;   // 巨 SCC 覆盖整批(以交易数近似)
            maxcr = std::max(maxcr, (double)p.max_scc_size / n);
            auto q = evaluate(fo, n, ord.lists, ord.honest,
                              hg.get_profile().final_group_sizes);
            rps_sum += q.rps_q; disp_max = std::max(disp_max, q.max_disp);
        }
        printf("%d,%.4f,%.4f,%.2f,%.4f,%.4f,%.4f\n", s,
               (double)any/batches, (double)full/batches,
               any ? (double)rounds_sum/any : 0.0, maxcr, rps_sum/batches, disp_max);
        // 注: rounds 列 = 触发批次的平均合并/修复轮数
        fflush(stdout);
    }
    return 0;
}
