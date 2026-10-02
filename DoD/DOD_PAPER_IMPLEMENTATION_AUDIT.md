# DoD paper–implementation consistency audit

Date: 2026-09-26  
Paper checked: *DAG of DAGs: Order-Fairness Made Practical*, Proc. ACM Manag. Data, Vol. 3, No. 6, Article 312 (December 2025)  
Implementation checked: `Order-Fairness-min/DoD_HS` and `Order-Fairness-min/simulations`

## Executive conclusion

`DoD_HS` is a working **DoD-inspired ordering layer carried by ordinary HotStuff**. It is not a reproduction of the paper's complete DoD protocol. The local/global graph core implements several important DoD rules, and it passes the current unit and 4-node smoke tests. The paper's Narwhal/Tusk DAG, worker sharding, PoA availability, digest/ACK synchronization, INFO-REQ path, and execution-queue blocking semantics are not implemented.

The strongest accurate label is:

> DoD ordering core on HotStuff, with ordinary HotStuff QC as the proposal/commit carrier.

It must not be reported as a full DoD/Narwhal/Tusk reproduction or as evidence for Theorem 4.1.

## Status legend

- **MATCH**: the relevant ordering rule is implemented with the same observable semantics.
- **ADAPTED**: the rule is represented, but the protocol carrier or state machine is changed.
- **PARTIAL**: the core idea exists, but an important state/edge case is missing.
- **MISSING**: no corresponding implementation is on the active DoD-HS path.
- **DEFECT**: the implementation contradicts an explicit paper requirement.

## Mechanism-by-mechanism audit

| Paper mechanism | Paper evidence | C++ implementation | Python implementation | Status | Consequence |
|---|---|---|---|---|---|
| Receive-order, data-dependent fairness | Sec. 2.2 and Sec. 3; `/private/tmp/dod.txt:483-556` | `LocalOrderDAG::create_dag()` uses receive position plus read/write dependency metadata: `src/graph.cpp:320-369` | Accepts permutations or dependency DAGs: `simulations/dod_hs.py:79-119` | **ADAPTED** | Dependency-aware ordering is represented, but there is no paper-level client multicast to same-shard workers. |
| Local transaction state: unseen, unprocessed, processed | Algorithm 1, `/private/tmp/dod.txt:650-699` | `on_local_order()` builds one batch DAG: `src/consensus.cpp:662-713`; it does not implement all three paper states and their per-worker round logic | Only receives already-constructed local evidence: `simulations/dod_hs.py:241-257` | **PARTIAL** | Cross-round replay and missing-edge weight accumulation are not modeled as in Algorithm 1. |
| Same-ID worker communication | Algorithm 1 and Sec. 4, `/private/tmp/dod.txt:594-611`, `670-699` | LocalOrder is sent to the current HotStuff leader: `src/hotstuff.cpp:489-500` | No network/message simulation | **ADAPTED** | Leader collection replaces worker-to-same-ID-worker dissemination. |
| Collect `n-f` local orders | Algorithm 2 input and `/private/tmp/dod.txt:710-717`, `813-815` | `on_receive_local_order()` triggers at `config.nmajority=n-f`: `src/consensus.cpp:716-790`; `fair_propose()` consumes cached DAGs: `src/consensus.cpp:799-812` | `DoDHotStuffSorter` enforces `n-f`: `simulations/dod_hs.py:405-440` | **MATCH / ADAPTED** | The quorum count matches, but the authenticated worker/PoA collection does not. |
| Fixed threshold | Algorithm 2 lines 1–3, `/private/tmp/dod.txt:813-820` | `n-2f` in `src/graph.cpp:73-81` and `src/consensus.cpp:928-934` | `dod_thresholds()` and labeling: `simulations/dod_hs.py:60-76`, `299-307` | **MATCH** | Correct for the supplied local-DAG evidence. |
| Pending threshold | Algorithm 2 lines 4–5, `/private/tmp/dod.txt:813-820` | `n(1-gamma)+f+1`: `src/consensus.cpp:936-946`, used at `src/graph.cpp:73-105` | Same formula: `simulations/dod_hs.py:60-76`, `299-323` | **MATCH** | The integer-count interpretation is consistent with the paper predicate. |
| Pairwise weighted edge selection | Algorithm 2 lines 10–22, `/private/tmp/dod.txt:826-838` | Stronger direction is inserted only at/above threshold; unresolved observations become missing: `src/graph.cpp:84-105` | Same rule: `simulations/dod_hs.py:309-323` | **MATCH** | Equal opposing evidence remains unresolved, as in the algorithm. |
| Missing-edge store across rounds | `/private/tmp/dod.txt:657-661`, Algorithm 2 lines 11–22 and 39–40 | Block `e_missing` is serialized and cached: `src/entity.cpp:45-57`, `src/consensus.cpp:243-258`; `fair_update()` separately derives `e_update`: `src/consensus.cpp:834-900` | Missing pairs are returned as telemetry only: `simulations/dod_hs.py:375-382` | **PARTIAL** | The implementation carries pair sets, but not the paper's persistent per-edge weights and worker-level update protocol. |
| SCC condensation | Algorithm 2 line 23 and `/private/tmp/dod.txt:762-778` | Kosaraju SCC and condensation reachability: `src/graph.cpp:108-201` | Deterministic Kosaraju and component graph: `simulations/dod_hs.py:165-206`, `325-384` | **MATCH / PARTIAL** | SCC members are exposed as `batches`, but the batch object is not part of the HotStuff block payload. |
| Pending pruning | Algorithm 2 lines 24–27 and `/private/tmp/dod.txt:779-789` | Reverse reachability from fixed components: `src/graph.cpp:166-187` | Same path-based retention: `simulations/dod_hs.py:342-359` | **PARTIAL** | This implements reachability-to-fixed semantics; the pseudocode's literal direct-edge condition is not represented separately. |
| Exclude missing pairs inside one SCC | `/private/tmp/dod.txt:790-796`, Algorithm 2 lines 28–30 | No SCC-membership check when copying `missing`: `src/graph.cpp:203-209` | Same missing filter: `simulations/dod_hs.py:375-382` | **DEFECT** | A weak direct observation between two transactions already in one SCC can be incorrectly reported as missing. |
| DAG transitive reduction | Algorithm 2 line 31 and `/private/tmp/dod.txt:797-806`, `849-850` | A legacy `TransitiveReduction` class exists at `src/graph.cpp:372-402`, but `build_dod_global_order()` and `fair_propose()` never call it | No transitive-reduction function/call in `simulations/dod_hs.py` | **MISSING** | The returned graph can contain redundant reachability edges and is not `G_T` from the paper. |
| Global-order digest, ACK, INFO-REQ | Algorithm 2 lines 32–48 and `/private/tmp/dod.txt:879-906` | No DoD digest/ACK/INFO-REQ flow; only HotStuff proposal/block transport | Not simulated | **MISSING** | Transaction availability and cross-replica graph synchronization are outside the implementation. |
| PoA and Narwhal DAG construction | Sec. 4.3 and Algorithm 3, `/private/tmp/dod.txt:908-918`, `932-937` | Ordinary HotStuff QC/three-chain path in `src/hotstuff.cpp` and `src/consensus.cpp` | Not simulated | **ADAPTED / MISSING** | QC authenticates the HotStuff block, but this is not DoD's PoA-backed Narwhal availability proof. |
| Execution queue blocks on unresolved missing edges | Algorithm 3 lines 5–18 and `/private/tmp/dod.txt:919-958` | `fair_finalize()` only checks `e_missing`/weak connectivity and returns the graph: `src/consensus.cpp:460-483`; committed graphs go directly to `ThreadPool`: `src/consensus.cpp:329-353` | `flattened_order()` exposes batches for metrics: `simulations/dod_hs.py:138-162` | **MISSING** | There is no persistent FIFO execution queue that stops at the first unresolved graph. |
| Concurrent execution of a DAG | `/private/tmp/dod.txt:955-966` | `ThreadPool` performs dependency-count execution: `src/consensus.cpp:88-177` | No concurrent executor | **ADAPTED** | It can execute a committed graph in parallel, but not the paper's queue-plus-missing-edge finalization semantics. |
| Theorem 4.1 system condition | `/private/tmp/dod.txt:489-515`, Theorem 4.1 `/private/tmp/dod.txt:1036-1044` | Uses ordinary HotStuff `f=floor((n-1)/3)` and only computes thresholds: `src/consensus.cpp:913-956` | Validates ordinary HotStuff fault bound: `simulations/dod_hs.py:52-76` | **MISSING** | No feasibility guard for `n > 4f/(2gamma-1)`; 4 nodes, `f=1`, `gamma=1` gives `4>4`, false. |

## Important concrete mismatches

### 1. The implementation has no transitive reduction on the active path

The paper requires `G_T = TransitiveReduction(G_C)` after SCC condensation. The C++ helper is dead with respect to the DoD-HS path: `build_dod_global_order()` returns at line 233, while the helper is only defined later and never invoked. The Python result has the same behavior.

Minimal Python probe on two identical local DAGs with `a->b`, `b->c`, and `a->c`:

```text
returned graph: a -> {b,c}, b -> {c}, c -> {}
paper-required reduction: a -> {b}, b -> {c}, c -> {}
```

### 2. SCC-internal missing pairs are not filtered

The paper explicitly says missing pairs inside one cycle need not be tracked. The active filter only checks that both vertices survive and that neither direct edge exists (`src/graph.cpp:203-209`; Python equivalent `simulations/dod_hs.py:375-382`). It does not check whether both vertices belong to the same SCC.

A four-cycle with one low-weight chord produces `scc_count=1` and still returns the chord as `missing_edges=[('a','c')]` in the Python mirror. The C++ core uses the same filtering logic.

### 3. The `batches` result is telemetry, not execution state

`DoDGlobalOrderResult` has `batches`, but `fair_propose()` returns only `(graph, missing_edges)` (`src/consensus.cpp:799-831`). `Block` serializes only the graph, `e_missing`, and `e_update` (`src/entity.cpp:34-60`). The commit path then gives a flat transaction graph to `ThreadPool` (`src/consensus.cpp:329-353`). Thus SCC batch boundaries are not explicit consensus/execution objects.

## Paper experiment alignment

The DoD paper evaluates a Narwhal/Tusk implementation, explicitly modifying Narwhal's worker/mempool and processing global-order graphs in Tusk's DAG (`/private/tmp/dod.txt:1060-1079`). The main setup reports end-to-end client measurements on CloudLab (`/private/tmp/dod.txt:1114-1129`), uses `n=5,f=1,gamma=1` for the batch-size experiment (`/private/tmp/dod.txt:1131-1138`), and varies `gamma` with `n=5,6,9,21,41` for `f=1` (`/private/tmp/dod.txt:1309-1316`).

The current local test is different:

- 4 ordinary HotStuff replicas, `f=1`, `gamma=1`, local loopback, `--notls`.
- It verifies process startup, HotStuff block progress, one fairness-stage invocation, and client confirmations.
- It does not reproduce the paper's worker/shard/DAG/PoA/Tusk pipeline or its CloudLab throughput/latency measurements.
- The condition needed by DoD's fairness theorem is not met: `n > 4f/(2gamma-1)` becomes `4 > 4`.

## Table-2 comparison scope

`simulations/table2_compare_dod_hs.py` compares Themis, canonical FlashOrder, and DoD-HS on the same recovered FlashOrder Table-2 traces. This is useful for ordering-core comparison, but it is **not a DoD-paper Table 2** and it is not HotStuff E2E performance. The script itself states that the metric definitions and traces are shared (`simulations/table2_compare_dod_hs.py:3-10`, `52-65`, `114-180`).

One-trial run on the three configured traces (`400` transactions, `30` inducing transactions, network advantage `0.3`) produced:

| gamma | n | protocol | RPS | Cov | MaxDisp | MaxGrp | graph edges | groups | Python core time |
|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 0.80 | 41 | Themis | 0.8515 | 0.4700 | 172 | 252 | 79,800 | 13 | 0.98 s |
| 0.80 | 41 | FlashOrder | 0.8668 | 0.8029 | 85 | 62 | 583 | 35 | 0.82 s |
| 0.80 | 41 | DoD-HS | 0.7192 | 0.4700 | 167 | 252 | 43,269 | 13 | 1.97 s |
| 0.51 | 401 | Themis | 0.9944 | 0.9841 | 20 | 26 | 79,800 | 180 | 0.41 s |
| 0.51 | 401 | FlashOrder | 0.9961 | 0.9202 | 8 | 62 | 188 | 20 | 0.98 s |
| 0.51 | 401 | DoD-HS | 0.9944 | 0.9841 | 20 | 26 | 78,646 | 180 | 19.48 s |
| 1.00 | 41 | Themis | 0.6895 | 0.5034 | 126 | 145 | 79,800 | 13 | 0.36 s |
| 1.00 | 41 | FlashOrder | 0.6904 | 0.5902 | 111 | 123 | 480 | 32 | 0.67 s |
| 1.00 | 41 | DoD-HS | 0.6856 | 0.5034 | 126 | 145 | 58,195 | 13 | 1.23 s |

These values are from `/private/tmp/dod_table2_compare_20260926/table2_themis_flashorder_dod_hs_trials.csv`. They show that the current DoD-HS ordering core can be compared on a paired trace, but the `execution_time` column is Python graph-construction time, not throughput, latency, or consensus cost.

## Tests actually run

| Test | Result | What it establishes |
|---|---:|---|
| C++ `ctest --test-dir build --output-on-failure` | **1/1 passed** | Current graph/cache unit test target passes. |
| Python `unittest` for `test_dod_hs.py` | **5/5 passed** | Python core mirrors the tested C++ graph semantics for cycles, pruning, missing pairs, quorum selection, and ordering/DAG input. |
| `ITER=120 CLIENT_SECONDS=25 ./test_tps.sh` with 4 local replicas | **PASS** | `replicas_listening=4/4`, `full_blocks=4`, `fairness_stages=1`, `client_confirmations=100`. |

The first sandboxed E2E attempt was blocked before startup by macOS socket permission (`ConnPoolError: unable to listen: Operation not permitted`); the same script succeeded once local loopback permission was available. The successful E2E run remains a HotStuff carrier smoke test, not a proof of DoD fairness.

## Claim boundary

Safe claim:

> We implemented and tested a DoD-inspired fixed/pending, weighted-edge, SCC-based ordering core and carried its graph through ordinary HotStuff; the C++ 4-node local smoke test and Python/C++ core tests pass.

Unsafe claims for the current tree:

- “complete DoD reproduction”;
- “Narwhal/Tusk or PoA availability reproduced”;
- “Theorem 4.1 validated by the 4-node run”;
- “the current Python `execution_time` is TPS/latency”; or
- “the returned graph is the paper's transitive-reduced `G_T`.”

