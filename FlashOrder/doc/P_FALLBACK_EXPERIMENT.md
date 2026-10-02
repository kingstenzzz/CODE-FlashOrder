# flashorder-P fallback 触发负载实验设计（本地 9 节点）

## 1. "fallback" 的确切含义

flashorder-P（preference-disagreement 变体）**没有** coverage-repair fallback
（那是 `flashorder-CCOF-R-fallback` / CCOF 的机制，见 `remote-test/KD_SWEEP_README.md:47`）。
PD 代码里真正的 fallback 是排序核的 **SCC 环回退**：

- 位置：`include/hotstuff/flashorder.h` → `topological_sort()`（L319–375）。
- 机制：簇间偏好图（`build_hypergraph`，margin=1.2）出现环时，普通拓扑序不存在，
  退化为 Tarjan SCC 合并 + `order_within_scc()` 的 net-score 启发式内部序。
- 语义：环上的簇被强制并成一个 batch，公平性粒度退化 —— 即 R5-W3 关心的退化行为。

### 判据（日志可观测）

```
[[HyperGraph]] SCC stats: total=%d, multi=%d, max_size=%d     # flashorder.h:353
[[HyperGraph-P]] D_t=%.6f K_D=%.3f delta=%d                   # flashorder.h:153
[[HyperGraph]] detect_clusters_refined DONE, clusters=%lu     # flashorder.h:186
```

**fallback 触发 = `multi > 0`**（存在尺寸 ≥2 的 SCC）。
逐块 fallback 率 = multi>0 的块数 / 有排序输出的总块数。

## 2. 触发机理 → 自变量选择

环需要簇间偏好出现 A→B→C→A（每条边都满足 1.2× margin）。这要求不同副本的
OrderedList（本地时间戳序）在热点交易上**系统性分歧**。三个杠杆：

| 杠杆 | 参数 | 机理 |
|---|---|---|
| 负载争用 | `SB_SKEW=0.99, SB_PROB=0.95` + 高 `max_async` + burst | 热点账户交易并发到达，各副本到达序差异被放大 |
| 网络乱序/时钟 | `reorder_proxy.py`（test/ 已有）、`tc netem` 每副本抖动、或 per-replica 时钟偏移 LD_PRELOAD | 直接制造副本间时间戳序分歧（本地 loopback 必需） |
| 协议参数 | γ↓（threshold = n(1−γ)+f+1 变小 → 更多 pair 过阈 → 双向边↑）；k_d↓（δ_D=1 → 单例簇多 → 簇间边多 → 环概率↑；k_d 大则单簇塌缩，H=1，不可能 fallback） | 控制"分母"——簇数与边的密度 |

**核心假设（实验要回答的问题）**：本地 loopback 上 9 副本时钟/到达序高度一致，
纯负载（即使 0.99 skew）也推不动 fallback；必须叠加乱序/时钟注入。
若验证成立，结论是"PD 的 fallback 由网络抖动驱动而非纯负载"。

## 3. 部署（本地 9 节点）

仿照 `flashorder-P/test_flashorder.sh` 的 loopback 模式：

```bash
cd flashorder-P
printf '127.0.0.1\n%.0s' {1..9} > _ips_9.txt
python3 scripts/gen_conf.py --prefix hotstuff --ips _ips_9.txt \
    --keygen ./build/flashorder-P-keygen --tls-keygen ./build/flashorder-P-tls-keygen \
    --block-size 200 --nworker 8 --repnworker 8 --clinworker 4 \
    --sb-users 100000 --sb-prob-choose_mtx $SB_PROB --sb-skew-factor $SB_SKEW \
    --fairness-parameter $GAMMA --pace-maker rr
# 启动 9 副本：FLASHORDER_P_KD=$KD ./build/examples/flashorder-P-app --conf hotstuff-sec$i.conf
# 客户端：--iter -1 --max-async $ASYNC，跑 30s
```

注意：非 TLS 部署需去除 conf 中的 tls 行（见 AGENTS.md）；保活性必须 `--pace-maker rr`。

## 4. 实验矩阵（先粗后细，每点 3 次重复、30s）

| 组 | SB_SKEW / SB_PROB | γ | k_d | max_async | 网络注入 | 预期 |
|---|---|---|---|---|---|---|
| A 基线 | 0.5 / 0.9 | 0.8 | 1 | 800 | 无 | fallback=0（阴性对照） |
| B 热点 | 0.99 / 0.95 | 0.8 | 1 | 3200 | 无 | D_t↑、簇变粗，fallback 仍≈0 |
| C 热点+低γ | 0.99 / 0.95 | 0.6 | 1 | 3200 | 无 | 首次出现 multi>0（若 B 为 0） |
| D 热点+乱序 | 0.99 / 0.95 | 0.6 | 0 | 3200 | reorder_proxy 或 netem 5ms±5ms 按副本 | 稳定触发（每块 multi>0） |

可选第 5 组（隔离因果）：A 参数 + 仅网络注入 → 若 fallback 出现，证明抖动是充分条件。

网络注入三选一（按实现成本递增）：
1. `tc netem delay 5ms 5ms 25%` 加在 loopback 上按副本差异化（需要 veth/netns，最真实）；
2. `test/reorder_proxy.py` 在 client→部分副本路径上缓存重发热点交易（已有工具）；
3. per-replica 时钟偏移：改 `test/mock_time.c` 为"偏移量"而非"冻结时间"（冻结会杀死 HotStuff 定时器），LD_PRELOAD 注入 ±10ms 偏移。

## 5. 算法级预扫（秒级，先于 e2e）

用现成工具确认参数方向，避免 e2e 瞎扫：

```bash
cd flashorder-P
./tools_kd_quality --adv --n 200 --nrep 9 --f 2 --gamma 0.6   # Condorcet 模式
./tools_phi_sweep                                                # k_d×noise×γ 的簇/SCC 统计
```

看 `n_sccs / multi-SCC / max_scc_size` 随 γ、noise、k_d 的走向，选分歧最大的组合进组 C/D。

## 6. 采集与分析

每副本日志解析（python，嵌进运行脚本）：

- 逐块：`SCC stats` 行的 `total/multi/max_size`；`D_t` 行的 D_t、delta；`clusters=%lu`
- 汇总指标：
  - **fallback_rate** = Σ(multi>0) / Σ(blocks)
  - **fallback_depth** = mean/max(max_scc_size | multi>0)
  - D_t 分布（p50/p95）、簇数分布
  - TPS / 延迟照旧（thr_hist.py），确认注入没有把系统打挂
- 预期图：fallback_rate 与 D_t 的散点（4 组各一色），验证 D_t 是否为 fallback 的有效前置指标。

## 7. 结果解读的两种走向

- **B/C 已触发**：负载本身（热点+低γ）即可推动 PD 走 SCC fallback → k_d 部署建议有实际意义（避开低γ+高争用组合）。
- **只有 D 触发**：PD fallback 纯粹由副本间时钟/乱序驱动 → 论文里可写"PD 在一致时钟的网络中退化为线性序，fallback 仅在抖动下出现"，并用 D 组量化退化程度（max_scc_size）。

## 8. 算法级负载扫描结果（2026-09-29，n=200, nrep=9, f=2, 5 repeats）

工具：`tools_kd_quality`（`--mode random/condorcet/heavy`），fallback 判据为 stderr
`SCC stats: multi>0`。数据：`remote-test/pd_fallback_sweep.csv`（汇总）、
`remote-test/pdalgo_<mode>_g<γ>_noise<n>.csv`（质量）。

### 8.1 什么负载触发 fallback

| 负载 | fallback_rate | 备注 |
|---|---|---|
| random（任意 noise/γ/k_d） | **0** | 噪声再大也不触发 |
| **heavy**（任意参数） | **0** | 单向量分歧（抢跑打包）不产生环，D_t≈0.084 与 random 同级 |
| **condorcet + k_d=0** | **100%**，max_scc 26–31 | 唯一稳定触发点 |
| condorcet + noise=50 + γ=0.8 + k_d∈[0.075,2] | 20%，max_scc=4 | 边缘触发区 |

结论：PD 的 SCC fallback 需要**双向系统性分歧**（Condorcet 式），单向偏差
（heavy/MEV 抢跑）和随机噪声都不足以成环。k_d>2 后簇粗化、边变稀，环消失。

### 8.2 发现并修复的正确性 bug（fallback 路径丢交易）

Condorcet + k_d=0 下最终序只覆盖 75/200 交易（`cov_check.cpp` 复现）。根因：
`build_scc_condensation()` 中 `scc_indeg` 按**重复边**累加而 `scc_adj` 事后**去重**，
Kahn 每条去重边只减一次 → 目标 SCC 的 indeg 永不归零 → 排序中途卡死，
之后的交易被静默丢弃。仅当存在 multi-SCC（即 fallback 触发）时同一源 SCC
会发出重复 condensation 边，故正常负载从不暴露。

修复（已合入 `include/hotstuff/flashorder.h`）：先对 (source_scc, target_scc)
对去重，再统计 indeg。修复后 fallback 场景 200/200 全覆盖，
`frac_robust_pair` 异常（k_d=0 行 0.152 vs 其他 1.0）同步消失。

### 8.3 遗留问题（未修）

`order_within_scc()` 在 multi-SCC 内部排序时把**超边下标当交易下标**查 pref
矩阵（[flashorder.h](../include/hotstuff/flashorder.h) `topological_sort` 调用处），
SCC 内簇间顺序实为无关交易的偏好值——只影响环内序质量，不影响覆盖数。
若要严谨可改为按簇内 canonical_pos 均值比较。

### 8.4 e2e 实验的参数建议（更新第 4 节矩阵）

算法级结果表明：要在 9 节点 e2e 中复现 fallback，需同时
(a) 注入双向分歧负载（复用 `remote-test/p_attack_injector.py` 的 Condorcet 口径），
(b) `FLASHORDER_P_KD=0`（或 γ=0.8+高噪声边缘区）。
纯 SmallBank 热点负载（B/C 组）预期 fallback=0，可作为阴性对照保留。

## 9. 本地 9 节点 e2e 结果（2026-09-30，loopback, block=200, async=1600, 20s/点）

工具：`flashorder-P/e2e_fb/run_p_fallback_e2e.sh`（部署/采集）+
`remote-test/p_fallback_stats.py`（字段分析器，与远程脚本共用）。
数据：`flashorder-P/e2e_fb/runs/kd*_{skew}*/`。二进制含 condensation 修复与 STAGE_PROF 埋点。

| k_d | skew | N | qSCC-any | qSCC-full | max_scc(触发时) | qSCC ms (med) | propose trig/nontrig | TPS | lat p50 |
|---|---|---|---|---|---|---|---|---|---|
| 1.0 | 0.5 | 2318 | 0% | 0% | — | 0.31 | –/1.06 ms | 22,003 | 69 ms |
| 1.0 | 0.99 | 2125 | 0% | 0% | — | 0.32 | –/1.09 ms | 20,161 | 75 ms |
| 0.0 | 0.5 | 2260 | **3.5%** | **3.5%** | 200 (全块) | 0.09 (trig) | 1.09/0.89 ms | 21,447 | 71 ms |
| 0.0 | 0.99 | 2268 | **1.5%** | **1.5%** | 200 (全块) | 0.08 (trig) | 1.12/0.97 ms | 21,523 | 71 ms |

### 9.1 结论

1. **默认 k_d=1 在真实 9 节点 loopback 下 0 触发**——与算法级预言一致（loopback
   时钟/到达序高度一致）。热点负载（skew=0.99）单独不触发。
2. **k_d=0 时持续触发（1.5–3.5%），且全部是全合并**（200 个单例簇塌进单一
   SCC，整块变一个 batch）。触发均匀分布于整个运行窗口（~4 块/秒），非预热假象。
   机理：真实块中各副本 OrderedList 可见集不同（部分可见性），弱边即可成环；
   δ_D=1 的 200 单例簇让环扩散成巨 SCC。skew=0.5 触发率反而高于 0.99
   （热点交易被所有副本同序看到，可见性分歧更小）。
3. **性能影响可忽略**：触发块排序耗时仅 +0.15~0.2 ms（propose 1.1 vs 0.9 ms），
   TPS/延迟无差别。有趣的是触发块的 SCC 阶段更快（0.08 vs 0.3 ms）——
   单一巨 SCC 的 condensation 平凡（1 节点无边），比 200 节点 DAG 处理便宜。
4. **merge_rounds 恒为 1**（PD 单遍 Tarjan+合并，无迭代修复轮）。
5. e2e 延迟对照（逐 batch）受限于 client 无逐笔时间戳，当前用
   propose trig/nontrig 对照近似；差异 ~0.2ms，远小于 70ms 端到端延迟。

## 10. HC-20: 101/21-replica macro-cycle stress case（2026-09-30）

规格：n_rep=21 (f=7), batch=800, cycle length=20, 5 seeds × 10 batches/seed,
strict_repair=False（PD 默认），margin λ=1.2（内置），γ=0.8。
工具：`flashorder-P/hc20_bench.cpp` → `tools_hc20`；数据：`flashorder-P/hc20_results/`。

构造（不引入新攻击模型）：honest 副本 rank r 对 20-tx 窗口做循环旋转
（标准 Condorcet 构造），相邻 pair 边权 20:1 ≥ 阈值 n(1-γ)+f+1=13（q_γ admission ✓，
margin ✓）；窗口外 pair 走原 noise+byzantine 路径。对照臂同参数无旋转。

| 臂 | k_d | qSCC-any | qSCC-full | merge rounds | max-cluster ratio | RPS_q | MaxDisp |
|---|---|---|---|---|---|---|---|
| control | 1.0 | 0% | 0% | 0 | 0.0013 | 0.9415 | 0.185 |
| control | 0.0 | 0% | 0% | 0 | 0.0013 | 1.0000 | 0.000 |
| HC-20 | 1.0 | 0% | 0% | 0 | 0.0013 | 0.9723 | 0.091 |
| HC-20 | 0.0 | **70%** (60–80%) | 0% | 1 | 0.0135 | 0.9998 | 0.024 |

### 10.1 结论

1. **δ_D 自适应是环的第一道防线且是全或无门**：HC-20 把 D_t 推到 0.33，
   任意 k_d>0 → δ_D≥2 → 全批塌成 ~5 簇，环在簇化阶段被吸收，
   quotient 层从未见过环（qSCC-any=0）。k_d∈{0.075,0.15,0.3} 与 k_d=1.0
   结果完全相同——过渡是阶跃而非渐变。
2. **k_d=0（δ=1）时 quotient 环真实出现**（70% 批次触发），但损害完全局部：
   qSCC-full=0，最大合并簇只占 1.4%（≈11 簇/800 tx），RPS_q 仍 0.9998，
   MaxDisp 0.024。即 quotient-SCC closure 在真 hard cyclic input 下按设计
   工作：只合并环涉及的簇，不扩散。
3. merge_rounds 恒 1（PD 单遍 Tarjan）。
4. 对照臂在 k_d=1.0 时 RPS_q(0.94) 反而低于 HC-20(0.97)——异常，待查
   （怀疑窗口旋转改变了 cp 分布/簇边界，或 RPS_q 口径对 byzantine 弱多数
   pair 敏感；控制臂 MaxDisp 0.185 也高于 HC-20 的 0.091）。

### 10.2 strict_repair 开关的影响（C++ 已实现，2026-09-30）

实现：`topological_sort()` Kahn 之后可选端点合并（union-find 取更早位置），
直到每个 γ-certified pair（pref[a][b] ≥ threshold）被吸收进同组或在最终
线序中有前向路径。env `FLASHORDER_P_STRICT_REPAIR=1` 开启，默认关；
遥测 `PhaseProfile.repair_rounds/repair_merges` + `strict_repair:` 日志行
+ `final_group_sizes`（吸收语义指标）。工具 `tools_hc20 --strict`。
数据：`hc20_results/hc20_strict_matrix.csv`（7 臂 × 5 seeds）。
RPS_q 口径更新：同组 pair 记为满足（CR-γ-BOF 吸收语义），默认臂数字随之
从 0.94–0.97 修正为 ~1.0（原口径把批内乱序算违反）。

| 臂 | k_d | strict | qSCC-any | rounds(trig) | max-clus | RPS_q | MaxDisp |
|---|---|---|---|---|---|---|---|
| HC-20 | 1.0 | off | 0% | – | 0.0013 | **1.000** | 0.091 |
| HC-20 | 1.0 | on | 0% | 1 (merge2)† | 0.0013 | **0.983** | 0.097 |
| HC-20 | 0.0 | off | 70% | 1.00 | 0.0135 | **1.000** | 0.024 |
| HC-20 | 0.0 | on | 70% | 1.43 | 0.0135 | **0.980** | 0.024 |
| control | 1.0 | off/on | 0% | – | 0.0013 | 1.000 | 0.185 |
| control | 0.0 | on | 0% | – | 0.0013 | 1.000 | 0.000 |

† 遥测原文：rounds=1 merges=2 groups=3（bench 的 rounds 列只统计 SCC 触发
批次，此处从日志读取）。

结论：
1. **有影响，且方向是负的**：strict 开启后 RPS_q 全面下降（1.000→0.98），
   无论有无环、无论 k_d。机理：strict 修复的是 δ-聚类粗化对 γ-certified
   pair 的破坏，修复方式是并组——并组本身满足 certified pair，但打乱了
   更多数序 pair（未达 γ 阈值）的相对位置。
2. **无环干净输入下 strict 零成本**（control 两臂逐字相同，rounds=0）。
3. k_d=0 时 strict 平均 1.43 修复轮（>SCC 单轮 1.00），即修复会级联
   （第一轮合并引入新的组序冲突）。
4. 建议：论文将 strict_repair 定位为 **可选的安全加固 profile**
   （换 CR-γ-BOF 严格性，付 ~2% 多数序满足度 + 级联修复轮），默认关闭
   的定位与数据一致。

### 10.3 Algorithm 2 (StrictRepair) 忠实重实现（2026-09-30，替换 §10.2 的近似版）

§10.2 的 strict_repair 是线性化判据+批量合并的近似；用户给出论文
Algorithm 2 后重写为逐字对齐：词法最小 violation 元组
(key(Hi),key(Hj),id(txi),id(txj)) 逐个修、每轮从 P 重建 quotient 边、
合并后 fold min-key 非平凡 quotient-SCC 至无环（与 CCOF-R
enforce_coverage_fallback 同构）。修复过程中发现并修正近似版的两个
伪影：RPS_q "下降 2%" 实为线性化判据漏修所致；clusters=1 批次空输出
bug（strict_handled 标志）。

| 臂 | k_d | strict | qSCC-any | rounds(trig) | RPS_q | MaxDisp |
|---|---|---|---|---|---|---|
| HC-20 | 1.0 | off | 0% | – | 0.9999 | 0.091 |
| HC-20 | 1.0 | on | 0% | 2 (merges=2)† | **1.0000** | 0.091 |
| HC-20 | 0.0 | off | 70% | 1.00 | 0.9998 | 0.024 |
| HC-20 | 0.0 | on | 70% | **4.6** (3.6–6.0) | **1.0000** | 0.024 |
| control | 1.0/0.0 | on | 0% | – | 1.0000 | =off |

† 遥测：rounds=2 merges=2 clusters=3（无 SCC 触发，从日志读取）。

Algorithm 2 忠实版的结论（修正 §10.2）：
1. **strict 只有收益没有代价**：所有臂 RPS_q → 1.0000，MaxDisp 不变，
   max_cluster_ratio 不变（合并不扩散），干净输入零改动。
2. 代价体现在**轮数**：k_d=0 有环时逐元组修复级联至平均 4.6 轮
   （vs SCC closure 的 1 轮）——strict 的开销是修复轮的 O(rounds·H²)
   重图/闭包计算，不是排序质量。
3. rounds 口径：CCOF-R/PD strict = 逐 violation 的 while 轮数；
   PD 默认 = SCC closure 单轮。跨表引用需注明。
数据：`hc20_results/hc20_strict_matrix.csv`（7 臂 × 5 seeds）。

### 10.4 strict_repair 性能优化（2026-09-30，语义逐字保持）

优化前病态角（HC-20 攻击 + k_d=0 + strict）topo 中位 **453ms/批**
（最大 ~984ms）：瓶颈为 Algorithm 2 每轮的 O(H³) Floyd 传递闭包
（H≈800 单例簇）+ 每轮 O(n²) violation 扫描 + 每轮 O(n²) 随机访存
pref 重求和。四层优化（全部保持 Algorithm 2 精确语义）：

1. **admitted 对预计算**：pref≥threshold 的簇对只依赖 P，跨轮不变，
   每轮扫描 O(n²)→O(|admitted|)。
2. **簇间权重矩阵 W 增量维护**：W[i][j]=Σpref 一次性 cache 友好构建
   （逐 txn 行顺序累加），合并=行/列相加 O(H)；margin 邻接从 W 派生。
3. **稀疏对手表 nzr + 邻接增量维护**：合并只有 keep 的邻接可变
   （其余簇对 W 不变→边不变），he_adj O(deg) 修补替代 O(E) 全量重建。
4. **闭包懒计算**：fold/Tarjan 不需要可达性；仅在每轮 violation 扫描
   前，对 fold 后的无环商图做 Kahn 拓扑序 + 反向传播的精确 bitset
   传递闭包，O(E·H/64)。曾尝试跨轮增量闭包，但攻击负载下合并会
   翻转 margin 边方向（vanish），回退全量重算频繁，收益不稳，弃用。

**验证**（tools_hc20，5 seeds 逐 seed 对比）：rounds（6.00/4.14/3.62/
4.43/4.71）、merges、RPS_q、MaxDisp 与优化前逐字一致；控制臂输出与
strict=off 逐位一致；默认路径（strict off）未改动。

| 臂 | 优化前 topo | 优化后 topo | 加速 |
|---|---|---|---|
| 攻击 kd=0 strict | 453ms med（984ms max） | **28.5ms med（50.8ms max）** | ~16× |
| 攻击 kd=1 strict | 1.6ms med | 5.4ms | 略增* |
| 干净 strict | 0.01ms | 0.01ms | – |
| 攻击 kd=0 off | 2.6ms | 2.6ms | 未动 |

*kd=1 略增源于 W 构建与 admitted 预计算的固定 O(n²)（~2ms/批），
但在部署默认（k_d 自适应）下 strict 从不触发修复轮，5ms 级 propose
开销不构成瓶颈；病态角从 453ms→28.5ms 后不再有活性风险。
剩余 28.5ms 为"每轮精确闭包 + 合并内存操作"的 Algorithm 2 语义地板。

残留 O(H²) 上限的结构性护栏仍是 k_d 自适应：攻击推高 D_t→δ_D≥2→
H 压缩到个位数。strict profile 应与 k_d 自适应绑定部署。

### 10.5 γ 敏感性矩阵与机制修正（2026-09-30）

γ×k_d×strict 矩阵（HC-20 攻击，5 seeds）+ 插桩核验。**修正早先
"低γ→threshold低→噪声admitted"的错误机制**：threshold=(1−γ)n+f+1
随 γ 降低而**升高**（γ=0.6→16票 vs γ=0.8→12票），admitted 对数
三个 γ 几乎相同（~30万）。真实链条：γ 低 → threshold 严 → 区分性
偏好少 → canonical 位置并列多 → delta 聚类变粗（505 vs 788 簇）→
粗簇偏好求和放大攻击矛盾排名 → 商图巨型 SCC（初始 fold 505→90）→
塌缩后大量 admitted 对无前向路径 → 67–95 修复轮。

| γ | 初始簇 | post-fold | rounds | RPS_q(off→on) | MaxDisp(off→on) | topo |
|---|---|---|---|---|---|---|
| 0.6 | 505 | 90 | 81–148 | 0.91→1.00 | 0.74→0.18 | 90–229ms |
| 0.8 | 788 | 788 | 4.6 | 1.00 | 0.024 | 28.5ms |
| 1.0 | 782 | 781 | 0–4.5 | 1.00 | 0.024 | ≤28ms |

结论：γ<0.7 的代价来自 **γ×margin×聚类粒度的间接耦合**，不是
准入放宽。设计区间 γ≥0.8 下聚类近全单例，环局部化。数据：
`hc20_results/hc20_gamma_sensitivity.csv`。

### 10.6 四负载开销矩阵 + Condorcet 扫描（2026-09-30，优化版二进制）

e2e（9 节点 LAN，块=200，kd=1）：normal 与 heavy（ASYNC 3200 饱和，
延迟 68→135ms）下 strict 代价完全相同：propose +1.0ms/块，TPS -2.3%，
触发率 0%。饱和点由共识/网络决定，排序检查不在瓶颈路径。
Condorcet 扫描（tools_hc20，cycle∈{0,3,10,20}）：kd=1 开销 4.8–5.5ms
与 cycle 强度无关；kd=0 时 cycle=10 轮数 14.0 反超 cycle=20 的 4.6
（小环散布 vs 少数大环）；干净+kd=0 的 strict 固定检查 13.9ms vs
kd=1 的 0.01ms——k_d 对固定成本也有 1000× 杠杆。
数据：`hc20_results/hc20_condorcet_sweep.csv`、`e2e_fb/runs/{lan_*,heavy_*}`。

### 10.7 e2e Condorcet 攻击实验：evil-client 与双层环吸收（2026-09-30）

用 `examples/evil_client.cpp`（--cycles/--cycle-len 3/10/20，副本分三组
按轮换序接收同一组交易）在 9 节点 e2e 注入 Condorcet 偏好环。
**插桩证实攻击完全成功进入偏好层**：ordered list 位置 rep0..2=[t0,t1,t2]、
rep3..5=[t2,t0,t1]、rep6..8=[t1,t2,t0]，pref 呈完美环（4>3, 4>3, 6>1，
threshold=4, margin 4>1.2×3）。

**但 qSCC/strict 触发率 = 0%（kd=1 和 kd=0 均如此）**，机制为两层环吸收：
1. **kd=1 层**：δ_D=2 聚类直接把相邻交易并簇，环在商图形成前被吸收；
2. **kd=0 层（更强）**：轮换攻击完全对称 → 环上交易获得相同 canonical
   位置（都=1）→ gap=0 < δ=1 仍合并进同一簇 → 环由簇内 internal_order
   （净得分）确定性消解（n_txns=200 → 67 簇，multi=0 全程）。

结论：**对称 Condorcet 攻击在 e2e 中到不了 fallback/strict——canonical
位置平局本身就是环检测器**。strict/SCC fallback 只会被非对称攻击触发
（admitted 环 + 位置分化，需攻击者注入额外偏序信号），HC-20 算法级
基准合成的正是这种非对称环。e2e 攻击臂开销：propose +0.3ms（0.48→
0.80ms），2000 cycles 全部确认，活性无损。工具：`e2e_fb/run_evil_e2e.sh`。

### 10.8 λ (margin) 敏感性矩阵（2026-09-30）

λ 是簇间偏好图边准入的 margin（`s_ij > λ·s_ji` 才建边，原内置 1.2）。
为可配置化：`HyperGraph()` 构造读 `FLASHORDER_P_MARGIN`（下限 1.0，默认
1.2 语义不变）；`tools_hc20 --margin λ`。复现验证：λ=1.2 与 §10 逐字
一致（attack kd=0 qSCC=70%）。

扫描：λ ∈ {1.0, 1.05, 1.1, 1.2, 1.5, 2.0} × {attack, control} ×
kd ∈ {1.0, 0.0} + attack kd=0 strict 臂，γ=0.8, n=800, nrep=21, f=7,
cycle=20, 5 seeds × 10 batches。数据：`hc20_results/hc20_lambda_sensitivity.csv`
（逐 seed 原始行：`hc20_results/lambda_sweep_raw.log`）。

| λ | qSCC-any (atk, kd=0) | rounds off | rounds strict | RPS_q (off/strict) |
|---|---|---|---|---|
| 1.00 | 100% | 1.00 | 1.44 | 0.9998 / 1.0000 |
| 1.05 | 94% | 1.00 | 1.92 | 0.9998 / 1.0000 |
| 1.10 | 82% | 1.00 | 3.15 | 0.9998 / 1.0000 |
| 1.20 | 70% | 1.00 | 4.58 | 0.9998 / 1.0000 |
| 1.50 | 38% | 1.00 | 25.41 | 0.9998 / 1.0000 |
| 2.00 | 2% | 0.20 | 39.80 | 0.9998 / 1.0000 |

结论：
1. **λ 是 quotient 环频率的单调门**：attack+kd=0 下触发率 100%→2%
   （λ 1.0→2.0）。attack 边本身 20:1 对任何 λ<20 都存活，衰减来自
   noise/byzantine 弱边随 λ↑ 被剪除 → 环失去搭桥边。max_cluster_ratio
   同步降（1.5%→0.6%），qSCC-full 恒 0（环从不扩散）。
2. **λ 的真实代价转移到了 strict 修复轮**：λ↑ → margin 边稀 → 更多
   γ-certified pair 在塌缩序中无前向路径 → strict repair 轮数单调
   爆炸（1.4 → 39.8 轮）。λ↑ 省的是 SCC closure（每轮 O(H²)），换来
   的是 strict 级联（每轮重图+闭包）——**调高 λ 对 strict profile 是
   净负**。
3. **默认 λ=1.2 位于膝点**：SCC closure 触发率 70% 但每轮局部化
   （1 轮、1.4% 簇），strict 也仅 4.6 轮；λ=1.5+ 后 strict 成本
   5–8× 恶化而环频率收益边际递减。λ≤1.2 时 RPS_q/MaxDisp 对 λ 完全
   不敏感（0.9998/0.024）。
4. **kd=1 层对 λ 完全不敏感**：所有 λ 下 qSCC=0、逐字相同——δ_D
   聚类在 margin 准入之前就把环吸收，λ 只是 kd=0（δ=1）退化模式下的
   活跃参数。control 臂全部 0 触发，与 λ 无关。
5. 部署含义：默认 profile（kd 自适应 + strict off）下 λ 可在 [1.0,2.0]
   任意取值而质量不变；若启用 strict，应锁定 λ≤1.2。


### 10.9 Table 2 口径 λ 敏感性（2026-09-30，Python 仿真）

方法：复用论文 Table 2 对抗仿真（`simulations/table2_simulation.py` 负载与
评测逐字复用）：honest 排序 = exp(10ms 生成) + exp(100ms 网络)，f=2 个
byzantine 副本**整序反转** + 30 条 inducing 交易（仅 byzantine 可见），
robust-pair threshold = ⌊n(1−γ)⌋+f+1，20 trials，FlashOrder(HyperGraphSorter)。
配置对齐 Table 2：γ=0.80(n=101)、γ=0.51(n=401)、γ=1.00(n=101)。
改动：`HyperG.py` 的 `HyperGraphSorter` 增加 `margin` 参数（默认 1.2，
替换硬编码；C++ 侧对应 `FLASHORDER_P_MARGIN`）。
工具：`simulations/table2_lambda_sweep.py`；数据：
`hc20_results/table2_lambda_sensitivity.csv`。

**结果：λ ∈ [1.0, 2.0] 在全部 3 个 γ 配置下 RPS_q/MaxDisp 逐位不变**
（γ=0.80/0.51：RPS=1.0000、MaxDisp≤5；γ=1.00：RPS=0.9887、MaxDisp≤22）。

机理（插桩验证，非 bug）：
1. margin 代码路径正确：单元测试中簇间偏好比 1.5 时 λ≥1.6 边被剪。
2. Table 2 负载下 delta=5 聚类只产生 **4 个粗簇**（38/18/3/71），簇间
   偏好比极端悬殊（honest 多数在簇间近乎全票），6 条商图边在 λ=5 下
   都不翻转——f=2 的反转攻击 + inducing 太弱，造不出偏好比落在
   [1, λ] 带内的**争议簇对**。
3. 对照 HC-20（C++ PD，f=7/n=21 宏环攻击，kd=0）：λ 单调控制环触发率
   100%→2%。差异根源是攻击强度与簇粒度：λ 只在"簇间存在系统性
   双向分歧且强度接近"时才 binding。

结论：**Table 2 对抗口径下 λ 是 inert 参数**——FlashOrder 在该负载的
排序由 q_γ admission 与 delta 聚型决定，margin 层无争议可裁。λ 敏感性
只在强对称操纵（HC-20 类）+ 细簇（kd=0）时显现。论文表述：
"λ 在论文对抗仿真负载 [1,2] 区间内不改变任何公平性指标"。

### 10.10 Table 2 口径 × flashorder-P C++ 排序核（2026-09-30）

同 §10.9 负载/评测，但被测对象换成 **flashorder-P 的 C++ 生产排序核**
（`HyperGraph::finalize`，kd 自适应默认 1.0，margin 经
`FLASHORDER_P_MARGIN` 注入）。工具：`flashorder-P/table2_lambda_bench.cpp`
→ `tools_table2_lambda`；数据：`hc20_results/table2_lambda_cpp.csv`
（20 trials，γ 配置对齐 Table 2）。

| γ | n | RPS_q（所有 λ∈[1,2]） | MaxDisp mean/max（所有 λ） |
|---|---|---|---|
| 0.80 | 101 | 1.0000 ± 0 | 0.3 / 1 |
| 0.51 | 401 | 1.0000 ± 0 | 0.2 / 1 |
| 1.00 | 101 | 1.0000 ± 0.0001 | 0.4 / 1 |

结论：
1. **λ 在 Table 2 对抗口径下对 C++ 核同样完全 inert**——18 个
   (γ, λ) 点 RPS/MaxDisp 逐位不变（trial 负载跨 λ 复用，差异纯由
   margin 引起，为零）。
2. **C++ PD 核全面优于 Python 参考实现**：MaxDisp ≤1（Python 版
   3.6–11.6，γ=1.0 时 Python RPS 0.9887 vs C++ 1.0000）——canonical
   位置聚类 + δ_D 自适应在弱攻击（f=2 反转）下把诱导扰动完全吸收。
3. 跨实现一致性：margin 层 binding 需要的"簇间争议偏好比"在 Table 2
   负载下不存在（§10.9 插桩），该结论现在有 Python 与 C++ 双实现背书。
   λ 敏感性只在 HC-20 类强对称攻击 + kd=0 细簇时显现（§10.8）。

### 10.11 Table 2 口径的临界 λ*（回答"没有灵敏度?"，2026-09-30）

灵敏度存在，但临界点比设计区间高一个数量级以上。

**簇间偏好比分布**（Python 插桩，10 trials）：γ=0.8/n=101 下 36 个
双向簇对的 pij/pji 比值 min/med/p90/max = **5.8 / 30.7 / 49.5 / 50**；
γ=0.51/n=401 下双向对数为 0（全部单向一致）——margin 只有超过比值
才剪边，故 [1,2] 区间内零翻转。

**C++ 核临界扫描**（`tools_table2_lambda --lambdas 1.2,5,10,30,50,100`，
10 trials）：

| λ | γ=0.80 RPS / MaxDisp | γ=1.00 RPS / MaxDisp | γ=0.51(n=401) |
|---|---|---|---|
| ≤30 | 1.0000 / 0.3 | 1.0000 / 0.3 | 1.0000 / 0.2 |
| **50** | **0.9984 / 10.2** | **0.9611 / 62.5** | 1.0000 / 0.2 |
| 100 | 1.0000 / 0.3 | 1.0000 / 0.3 | 1.0000 / 0.2 |

结论：
1. **λ* ≈ 30–50**：Table 2 攻击制造的簇间比值上限 ~50（f=2 byzantine
   反转 + 诱导在 101 节点上的最大杠杆），margin 超过它才开始剪边。
   设计区间 [1,2] 与 λ* 之间有 >15× 安全裕度。
2. **退化非单调**：λ=50 最差（保留的恰是被攻击污染的强边，剪掉
   honest 弱序边 → MaxDisp 62），λ=100 反而恢复（商图全空，退化为
   canonical 位置序，攻击边也被剪掉）。若把 λ 当防御旋钮调大，
   会先经过一个**质量更差的中间带**——λ 不是越大越安全。
3. γ=0.51/n=401 对任何 λ 完全 inert（偏好全向一致，无双向簇对）。

### 10.12 用 flashorder-P 自身代码插桩确认 λ*（2026-09-30）

替换 §10.11 机理层的 Python 插桩：`tools_table2_lambda --instrument`
直接用 flashorder-P 的 `detect_clusters_refined()` 聚类结果与
`compute_preferences()` 的 pref 矩阵统计簇间比值（header 新增只读
访问器 `get_hyperedges()/pref_at()`）。

| γ | n | C++ 簇数 | 双向簇对 | 比值 min/med/max |
|---|---|---|---|---|
| 0.80 | 101 | 7 | 21 | **49.5 / 49.5 / 49.5** |
| 0.51 | 401 | **1** | 0 | —（全批单簇，商图空） |
| 1.00 | 101 | 26 | 325 | 35.7 / 49.5 / 51.7 |

边保留悬崖：γ=0.80 在 λ=50 处 21→0（γ=1.00 在 λ=50 处 325→1，λ=100→0）。

**机理完全闭合**：49.5 = (n−f)/f = 99/2 恰好是 f=2 反转攻击的理论最大
杠杆——byzantine 反转使每个 honest 一致 pair 的偏好恰为
99(honest):2(byz)。因此：
- λ* = (n−f)/f 是**攻击结构的解析函数**，非经验拟合：γ=0.80 下所有
  21 个争议簇对比值逐点等于 49.5，λ=49.5 处全部同剪（全或无悬崖，
  与 §10.1 的 δ_D 阶跃行为同构）；
- γ=0.51/n=401 时 C++ 聚类直接把整批合成 1 簇（H=1），margin 层
  结构性不存在；
- 设计区间 [1,2] 与 λ*≈50 之间裕度 **>24×**。注意 λ*=(n−f)/f 是
  反转攻击的杠杆上限；HC-20（§10.8）中 λ∈[1,2] 活跃的边是
  noise/byzantine **弱边**（比值落在 [1,2] 带内），与强边的 λ* 是
  两个不同的边缘群体——λ 的灵敏度谱 = 攻击在商图上留下的全部
  比值分布，[1,2] 内有弱边则敏感（HC-20），只有 ≥49.5 的强边则
  完全不敏感（Table 2）。

### 10.8 e2e HC-20 非对称攻击 + 21 节点两层触发矩阵（2026-09-30）

evil-client 新增 `--rot-mode rank`：复刻 HC-20 模型（每副本按自身序号
旋转 20-txn 窗口 → 非对称 Condorcet；原 `group` 模式为对称三组轮换）。
21 节点 LAN，kd=0/1 × strict off/on，600 cycles 全部确认（活性无损）。

**两层首次在 e2e 真实触发**（此前所有 e2e 负载两层均 0%）：

| 臂 | 默认层 | strict 层 | topo | propose |
|---|---|---|---|---|
| rank kd=0 off | **11.9%**（1 轮 closure，SCC≤10） | — | p50 0.07 / max 0.11ms | 0.83ms |
| rank kd=0 on | **15.3%**（SCC 中位 6） | **89.8%**（中位 50 轮，max 70） | **p50 3.41 / p90 8.55 / max 12.55ms** | 1.5–4.4ms |
| rank kd=1 on（部署） | **0%** | **0%** | p50 0.23 / max 0.38ms | 0.92ms |

对照：对称 group 模式 21n kd=0 仅 strict solo 层 58.6%（默认层 0%），
kd=1 全零。与算法级 HC-20（70% 触发/4.6 轮）方向一致；e2e 50–70 轮
最贵 12.6ms（块 200 txns、H 小于算法级 800）。

重测修正：9 节点诚实 kd=0 normal/heavy 两层 0%（每臂 ~3000 块），
早先"1.5–3.5% 偶发小环"不再复现（旧配置差异），该行作废。

触发矩阵终版：
- 诚实负载（任意节点数/网络/负载强度）：两层 0%；
- 对称环：canonical 位置平局吸收（kd=0/1 均 0%，默认层永不触发）；
- 非对称环：kd=1 免疫（δ_D 负反馈）；kd=0 两层全触发；
- γ<0.7 配置出界：巨型 SCC + 81–148 轮（算法级）。

工具：`e2e_fb/run_evil_e2e.sh`（ROTMODE=group|rank）、
`evil_client.cpp --rot-mode`。数据：`e2e_fb/runs/r21_hc20_*`、
`e2e_fb/runs/r9_*`、`e2e_fb/runs/r21_*`。
