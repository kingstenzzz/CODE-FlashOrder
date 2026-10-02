# #29 — 21 节点 PD 对抗性宏周期 E2E 包络实验

回应审稿人“默认 PD 路径在对抗输入下缺少 fairness-loss / cluster-size / MaxDisp 强界”的质疑。
本目录 = 真实 21 节点 receive-order 采集 + 离线 PD 重放，并验证 **C++ final-order hash == Python PD final-order hash**。

## 1. 方法

```text
21 个本地副本（loopback, n=21, f_struct=7, q_γ=8 at γ=1.0, strict_repair=False）
  └ evil-client 宏周期注入：spine + 20 个不等大小组，副本 i 看到组块旋转 i%20（cycle arm）
     或全部旋转 0（control arm）；单批 m ∈ {200,400,800}
  └ 副本端采集：每行 receive order（[ROW_DBG]，分片）
  └ 排序端采集：propose 时实际消费的行集合（[SORT_ROWS]）+ 最终序（[FINAL_ORDER_FULL]）
  └ 离线：同一批行 → pd_core（Python PD）重放 → 对比 sha256(final order)
```

排序路径（C++ 提案路径，未改动算法）：`preference → canonical positions → delta 聚类 → quotient 构建 → quotient-SCC merge → NPS 簇内精排`。

## 2. 主结果表（3 seeds，全部确定一致）

| m | arm | L_γ (strict) | L_γ (absorbed) | RPS_γ | C_max/m | D_max/(m−1) | final clusters | C++/Python hash |
|---:|---|---:|---:|---:|---:|---:|---:|---|
| 200 | control | 0.0000 | 0.0000 | 1.000 | 0.0050 | 0.0000 | 200 | 3/3 ✅ |
| 200 | cycle | 0.2093 | 0.0000 | 1.000 | **0.7400** | 0.6935 | 2 | 3/3 ✅ |
| 400 | control | 0.0000 | 0.0000 | 1.000 | 0.0025 | 0.0000 | 400 | 3/3 ✅ |
| 400 | cycle | 0.2177 | 0.0000 | 1.000 | **0.7375** | 0.6892 | 2 | 3/3 ✅ |
| 800 | control | 0.0000 | 0.0000 | 1.000 | 0.0013 | 0.0000 | 800 | 3/3 ✅ |
| 800 | cycle | 0.2219 | 0.0000 | 1.000 | **0.7375** | 0.6884 | 2 | 3/3 ✅ |

补充记录（cycle arm，每 seed 相同）：

| m | qualified pairs | violated (strict) | violated (absorbed) | propose ms | prefs/canon/clusters/hypergraph/topo ms | client confirm p50/p95 ms |
|---:|---:|---:|---:|---:|---|---|
| 200 | 19,900 | 4,165 | 0 | 2.17 | 0.31/0.04/0.07/0.05/0.02 | 1934 / 1934 |
| 400 | 79,800 | 17,370 | 0 | 5.71 | 1.15/0.14/0.17/0.13/0.01 | 1951 / 1957 |
| 800 | 319,600 | 70,915 | 0 | 13.60 | 4.03/0.53/0.95/0.72/0.02 | 1975 / 1981 |

cycle 臂簇结构始终是 2 簇：spine 簇 + 宏周期巨簇（m=200: 52+148；400: 105+295；800: 210+590）。

## 3. 假设检验结论

- **H1（cluster collapse）成立**：cycle 臂 C_max/m = 0.74 / 0.7375 / 0.7375，随 m 迅速饱和到 ≈3/4；control 臂恒为 1/m。对抗性 cyclic evidence 把整批组交易压成一个巨簇。
- **H2（qualified-pair loss）部分成立，且形态比预期更清晰**：strict 定义下 L_γ ≈ 0.209→0.222（随 m 微增）；**absorbed 定义下恒为 0** —— 即所有违反都发生在巨簇内部，跨簇没有违反。这直接说明损失不是“排序错乱”，而是**歧义区吞掉了整批组交易**，簇内由启发式决定相对序。
- **H3（normalized MaxDisp 随 m 增大）不成立**：0.6935 → 0.6892 → 0.6884，基本持平（略降）。退化是“一步到位”的簇坍缩，而不是位移随规模累积。

## 4. 可审计性：hash 等式成立所需的三个条件（重要）

实验过程中发现，C++/Python 一致性并不是自动成立的，需要：

1. **日志原子性**：13 KB 单行（21×m 行、m 个 final order hash）在并发写日志时会被截断/交织 → 必须分片（15 hash/行）。
2. **确定性 tie-break**：C++ 初始簇内排序原先只比较 net score，等分时依赖 `std::sort` 的不稳定行为 → 已改为 `(score desc, tx_id asc)`，与离线核一致（`include/hotstuff/flashorder.h`）。仅影响等分次序，不改变算法语义。
3. **重放必须使用排序时真实消费的行集合**：propose 在缓存达到多数派时立即触发，实测**只用到 15 行（不是 21 行）**；且 C++ 的 tx id 按行扫描的出现顺序分配，tie-break 依赖该标号 → 离线重放必须按 `[SORT_ROWS]` 给出的行集合与顺序重标号。

满足三条后，m=200/400/800 全部 3 seeds 的 `sha256(final order)` 完全相等（18/18）。

## 5. 参数边界（必须与结果一起读）

- **参与排序的行数 = 15（n=21）**：这是 C++ 提案路径的真实行为（多数派触发即排序），不是缺陷但会削弱阈值的可达性。
- **γ=0.6 在本设置下不可用**：f_struct=7 时 q_γ = ⌊21·0.4⌋+7+1 = 16 > 15 行 → 没有任何 qualified pair，L_γ 空洞。本实验采用 **γ=1.0 → q_γ=8**。
- **3 个 seed 是 3 次独立执行，但注入内容确定**：SmallBank 交易流确定，然而 loopback 下的
  receive order 存在运行间抖动，**三次的 final-order hash 互不相同**（每格 unique=3）；
  稳定的是**聚合指标**（L_γ、C_max/m、D_max/(m−1) 三次完全一致到 4 位小数）。
  因此应表述为「3 次独立执行下聚合指标稳定」，既不能说「逐位重放」，也不能当成统计样本。
- **client confirm latency ≈ 1.95 s** 是整批（单客户端一次性注入 21×m 条消息）的确认延迟，受共识与注入方式主导，**不能当作排序阶段延迟**；排序阶段延迟见 `propose_ms` 与其阶段分解。
- 无 WAN 延迟注入、无签名/网络故障注入；result 仅覆盖 loopback。


## 5.5 与论文定理的关系（用同一批数据验证，`verify_theorems.py`）

本实验的 L_γ 有两个口径，**必须与论文的定义对齐才不会误读**：

| 论文定义/定理 | 本数据上的检验 | 结果 |
|---|---|---|
| Prop. deterministic replay | C++ final order sha256 == Python PD 重放 | **18/18 相等** |
| Thm. NPS support preservation：簇大小 b，支持度 t > c(1−1/b) 的 pair 必须被正确排序 | 各簇达标 pair 的违反数 | **全部 0**（m=800 巨簇 28715/28715；m=400 295-簇 7448/7448） |
| CR-γ-BOF（batch 级：absorbed pair 允许 β 相等，跨簇 pair 必须正向） | 跨簇 admitted pair 违反数 | **全部 0** |

结论：本 workload 下默认路径**没有违反 CR-γ-BOF**（跨簇零违反），代价全部体现为**歧义区规模**（0.74），且歧义区内的行为**恰好落在论文 NPS 定理允许的范围内**（达标 pair 零违反）。
因此 21% 这个数字的准确表述是「**簇内相对 certificate 多数方向的严格序一致性损失**」，**不是** "21% fairness violation"——按论文 β 定义后者为 0。
`Thm. strict-mode coverage-repaired γ-BOF` 才是把 β 序保证覆盖到全部 admitted pair 的机制（需启用 strict mode），代价是定理自己声明的「worst case 整批合并」，本实验量化了该代价：C_max/m = 0.7375。


## 5.6 strict fallback 臂（`STRICT=1`）实测：本 workload 下是空操作

`FLASHORDER_P_STRICT_REPAIR=1` 的 gate 条件：对每个**有向** admitted pair (a,b)，若两笔分处
不同簇且 quotient 中不存在 ca→cb 的前向路径，则合并该对端点簇，直到不动点。

实测（m=200 / m=800, cycle 臂）：

| 指标 | 默认臂 | strict 臂 |
|---|---|---|
| `strict_repair: rounds/merges` | —（未启用） | **0 / 0** |
| final clusters | 2 | 2 |
| C_max/m | 0.74 / 0.7375 | 0.74 / 0.7375 |
| L_γ (strict) | 0.2093 / 0.2219 | 0.2093 / 0.2219 |
| C++/Python hash | 一致 | 一致 |

**为什么 gate 根本不触发（结构性原因，不是偶然）**：
1. 实测 propose 时只消费 `c=15` 行，而 `q_γ=8`；一对交易双向 admitted 需要 `2·q_γ = 16 > 15` 行，
   **在本配置下不可能出现跨簇的 both-admitted 冲突对**；
2. 单向 admitted 的跨簇对（spine 簇 ↔ 巨簇）方向与已有 quotient 边 spine→giant 一致，
   前向路径存在 → 无需修复（实测跨簇违反 = 0）。

**推论（可证伪）**：strict fallback 要真正发挥作用，需要 `c ≥ 2·q_γ`，即排序时必须汇集
**全部 21 行**（21 ≥ 16）。当前多数派即 propose 的实现使 `c≈15`，从结构上排除了修复触发。
换言之：**本 workload 下 strict 开关不改变结果；差异只会在"参与排序的行数"提高后出现。**

## 6. Claim 边界（与 spec §11 一致）

> FlashOrder-PD provides deterministic, auditable ordering and favorable empirical behavior on evaluated workloads, but arbitrary cyclic evidence can still create large ambiguity regions. The default path therefore does not claim a non-trivial universal bound on fairness loss, final-cluster size, or maximum displacement.

本实验**未**证明普适下界/上界；它给出的是：在已评测的 macro-cycle family、m ∈ {200,400,800}、n=21 下，默认 PD 会出现 ≈0.74 的簇坍缩与 ≈0.21 的 strict qualified-pair loss，且该行为在真实 21 节点部署上可复现、C++ 与离线 PD 逐位一致。

## 7. 文件

```
flashorder-P/e2e_fb/envelope21/
├── run_envelope.py       # 主驱动：部署/注入/采集/重放/指标（18 轮）
├── recheck_hashes.py     # 离线重算 C++/Python hash 一致性（不重跑集群）
├── check_replay.py       # 分歧定位（首个差异位、分数、簇结构）
├── verify_theorems.py    # 论文两条定理 + CR-γ-BOF 的数据验证
├── m{200,800}_cycle_s0_strict/  # strict fallback 臂证据（rounds=0 merges=0）
├── envelope_results.csv  # 18 轮明细
├── envelope_metadata.json
├── envelope_run.log
└── m{200,400,800}_{control,cycle}_s{0,1,2}/  # rep*.log + client.log 原始证据
```

改动过的源码：`flashorder-P/examples/evil_client.cpp`（宏周期注入、分片日志、客户端延迟）、
`flashorder-P/src/consensus.cpp`（ROW_DBG/FINAL_ORDER_FULL 采集）、
`flashorder-P/include/hotstuff/entity.h`（SORT_ROWS）、
`flashorder-P/include/hotstuff/flashorder.h`（确定性 tie-break）。

## 8. 复现

```bash
cd flashorder-P/e2e_fb/envelope21
python3 run_envelope.py          # 约 15 分钟，18 轮
python3 recheck_hashes.py        # 离线复核 hash 一致性
```
