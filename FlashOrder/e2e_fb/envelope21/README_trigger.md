# strict-repair 触发攻击 workload（最小构造）

回应「默认 PD 路径的 strict fallback 从未被实测」——设计一个**能真正触发
repair gate** 的对抗负载，并在真实 21 节点部署上验证。

## 1. 触发机制（一句话）

strict gate 只在「**有向 admitted pair (a,b) 分处不同簇、且 quotient 无 ca→cb
前向路径**」时 merge。要触发，需要构造一个 **both-admitted 对**（pref[a][b]≥q
且 pref[b][a]≥q）落在**不同簇**里（CP 差 ≥ Δ）——DAG 里不可能同时有两条反向
路径，故必然 merge。

**必要条件**：`c ≥ 2·q_γ`（c = 参与排序的行数）。之前的 envelope 实验
（γ=1.0，q=8）propose 只消费 c=15 < 16，双向 admitted 造不出来 → gate 恒空转。

## 2. 最小触发负载（n=21, m=7, γ=1.0, f=7, q=8）

21 行 × 7 交易，见 `trigger_w21_m7.txt`。核心是**一对 both-admitted 交易 (6,0)**：

- pref[6][0] = 8 ≥ q，pref[0][6] = 13 ≥ q（8+13=21，双向 admitted）
- CP(6) 与 CP(0) 差 ≥ Δ=2 → 落在不同簇
- 默认聚类 2 簇，quotient 边 0→1；(6,0) 方向 簇(6)→簇(0) 无路径 → **gate 触发**

n=9 版本见 `trigger_w9_m5.txt`（违反对 (3,0)，pref 4:5，q=4）。

## 3. 验证结果

### 3.1 C++ 排序核（`trigger_verify.cpp`，直接调 HyperGraph::finalize）

| 负载 | 默认 | strict |
|---|---|---|
| n=21 m=7 | rounds=0 merges=0，2 簇，序 0 1 2 3 4 5 6 | **rounds=1 merges=1**，1 簇，序 0 1 2 3 5 4 6 |
| n=9 m=5 | rounds=0，2 簇，序 0 1 2 3 4 | **rounds=1 merges=1**，1 簇，序 1 0 3 2 4 |

### 3.2 真实 21 节点部署（`run_trigger_e2e.py`，evil-client `--rows-file` 注入）

| 配置 | 参与排序行数 | 结果 |
|---|---|---|
| 默认 propose（多数派触发） | **15** | SCC total=1（已坍缩），gate **不触发** |
| `FO_WAIT_ALL_ROWS=1`（等全部 21 行） | **21** | `strict_repair: rounds=1 merges=1 clusters=1` ✅ |

这组对照**实测印证了 `c ≥ 2q_γ` 的必要条件**：c=15 时 both-admitted 造不出（无触发），
c=21 时才出现 both-admitted 对并触发 merge。

## 4. 结论

- strict fallback **不是死代码**：在满足 `c ≥ 2q_γ` 的负载下会真正 merge，
  把跨簇 both-admitted 对收进同一簇，恢复「每个 admitted 对要么 absorbed 要么
  有前向路径」的 CR-γ-BOF 不变量。
- 论文 `thm:coverage_repaired_fairness` 现在有了**实测支撑**：gate 触发时
  2 簇 → 1 簇，正是定理声明的「端点合并」行为。

## 5. 文件

```
flashorder-P/e2e_fb/envelope21/
├── trigger_w21_m7.txt / trigger_w9_m5.txt    # 带头的 workload（供 C++ verify）
├── trigger_w21_m7_client.txt / _w9_m5_client.txt  # 无头的 client 行文件
├── find_trigger.py          # 搜索最小触发负载（Python 忠实移植 strict gate）
├── run_trigger_e2e.py       # 真实部署驱动（--rows-file 注入 + FO_WAIT_ALL_ROWS）
└── live_w21_strict/ live_w21_strict_nowait/  # 21 节点原始日志证据

flashorder-P/trigger_verify.cpp               # C++ 排序核验证器（读 stdin）
```

改动：`examples/evil_client.cpp`（`--rows-file` 显式行注入）、
`src/consensus.cpp`（`FO_WAIT_ALL_ROWS` 等全部行，仅测试用）。

## 6. 复现

```bash
# C++ 核验证
cd flashorder-P/build
g++ -std=c++17 -O2 -I../include -I../salticidae/include -I../secp256k1/include -I.. \
    ../trigger_verify.cpp -o trigger_verify libhotstuff.a salticidae/libsalticidae.a \
    ../secp256k1/.libs/libsecp256k1.a -lcrypto -lpthread
./trigger_verify 1 < ../e2e_fb/envelope21/trigger_w21_m7.txt

# 真实 21 节点
cd ../e2e_fb/envelope21
python3 run_trigger_e2e.py w21_strict          # WAIT_ALL=1 STRICT=1 → 触发
WAIT_ALL=0 python3 run_trigger_e2e.py w21_nowait   # 默认 → 不触发
```
