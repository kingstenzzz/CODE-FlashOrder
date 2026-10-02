# Auncel — CSC Framework Implementation

Implements the Consensus with Sorting and Certification (CSC) framework from the Auncel paper. Built on libhotstuff with fair transaction ordering via Sort-Vote, OrderQC, and VVC protocols.

## Architecture

### Paper's 6-Stage CSC Framework → Code Mapping

The Auncel paper (§IV-B) describes a 6-stage consensus pipeline. The implementation maps these stages onto HotStuff's existing 3-step QC chain, adding CSC-specific phases for fair ordering:

| Paper Stage | Purpose | Code Implementation | QC Type |
|-------------|---------|---------------------|--------|
| **1. View-Change** | Select new leader, collect pending txs via VVC | `on_view_change()` → `do_send_new_view()` → `new_view_handler()` | Cert signs HQC hash |
| **2. Order-Propose** | TxSync (leader→replicas) + Sort-Vote (replicas→leader) | `on_propose()` → `do_send_tx_sync()` → `on_tx_sync()` → `do_send_sort_vote()` | Cert signs `hash(block_hash \|\| local_ordering)` |
| **3. Prepare** | Leader forms OrderQC from Sort-Votes, broadcasts block with OrderingMap; replicas verify and vote | `finalize_pending_proposal()` → `on_receive_proposal()` → `do_vote()` | **OrderQC** (Sort-Vote sigs) + **PrepareQC** (block vote sigs) |
| **4. Precommit** | Leader collects n-f Prepare-Votes → PrepareQC; next block carries this QC | `on_receive_vote()` → `qc->compute()` → `update_hqc()` | **PrepareQC** = HotStuff's standard QC |
| **5. Commit** | 3-chain rule triggers commit; replicas update LockedQC | `update()` → commit chain advances → `b_lock` updated | **LockedQC** = `b_lock` in HotStuff |
| **6. Decide** | Block committed, transactions executed with fair ordering | `exec_blk()` → `do_decide()` for each ordered tx | **CommitQC** = `decision=1` on Block |

**Key insight**: Stages 3-6 are folded into HotStuff's standard 3-step QC chain (`update()` function). The CSC-specific work is in stages 1-2 (VVC + Order-Propose) and the OrderQC formation in stage 3.

### Two Independent QC Types (Paper §IV-B, Fix #2)

1. **OrderQC** — proves n-f replicas agreed on their local orderings. Formed from individual Sort-Vote certs, each signing `hash(block_hash || local_ordering)`. Stored as `order_qc` on Block.
2. **PrepareQC / CommitQC** — standard HotStuff QC chain proving block consensus. These are the `self_qc` and `qc` fields on Block, following HotStuff's 3-chain commit rule.

### CSC Protocol Flow (3 phases per view)

```
Leader                Replicas
  |                       |
  | <-- TxSync(leader_id, block_hash, HighQC, curTxs) <-- |  Phase 1: TxSync
  |                       |
  | -- Proposal(block) -->|                              Leader proposes (empty ordering)
  |                       |
  | <-- SortVote(local_ordering, missing_txs, cert) <-- |  Phase 2: Sort-Vote
  |                       |
  | [WeightOrderer aggregates votes]                    Leader computes ordering
  | [forms OrderQC from n-f signatures]
  |                       |
  | -- Proposal(ordering_map, OrderQC) --> |             Phase 3: Order binding
  |                       |
  | [replicas verify OrderQC, execute block]            Consensus + commit
```

### Critical Data Structures

- **TxSync** (`auncel_msg.h`): `view`, `leader_id`, `leader_block_hash`, `high_qc`, `cur_txs` — also serves as the dynamic tx hash→ID mapping table (Fix #3: index in `cur_txs` = short binary ID)
- **SortVote** (`auncel_msg.h`): `replica_id`, `view`, `local_ordering` (full 256-bit hashes), `ordering_ids` (compressed uint32 indices into cur_txs, Fix #3), `missing_txs`, `cert` (signs `hash(block_hash || local_ordering)`, Fix #6)
- **NewView** (`auncel_msg.h`): `replica_id`, `view`, `high_qc`, `tx_list` (full pending tx pool from `decision_waiting` + uncommitted `receive_order_log`, Fix #8), `cert`
- **OrderQC** (`entity.h` Block field): `quorum_cert_bt` — aggregate of n-f Sort-Vote signatures (when certs sign matching hash); individual certs also forwarded in Proposal for replica-side verification (Fix #6)
- **Proposal** (`consensus.h`): carries `ordering_map` (final ordering for exec), `vote_replica_ids` + `vote_orderings` + `vote_certs` (per-replica Sort-Vote proof, Fix #6), block's `order_qc` via presence flag
- **`receive_order_log`**: `std::deque<uint256_t>` — accumulated received transactions for local ordering computation
- **`receive_order_set`**: `std::unordered_set<uint256_t>` — dedup companion for the log
- **`ordering_map`** (on Block): `std::vector<std::vector<uint256_t>>` — single entry containing the final WeightOrderer output (set by `finalize_pending_proposal()`, used directly by `exec_blk()` without re-running WeightOrderer, Fix #7)

### Source Files

| File | Responsibility |
|------|---------------|
| `include/hotstuff/consensus.h` | HotStuffCore class, Proposal/Vote/Finality structs, virtual method declarations |
| `include/hotstuff/hotstuff.h` | HotStuffBase (network layer), MsgNewView/MsgTxSync/MsgSortVote wrappers, VVC state |
| `include/hotstuff/liveness.h` | PaceMaker hierarchy: PMRoundRobinProposer (view rotation, VVC trigger, timer-based proposing) |
| `include/hotstuff/auncel_msg.h` | NewView, TxSync, SortVote serializable structs |
| `include/hotstuff/auncel.h` | WeightOrderer (weight-based transaction ordering algorithm) |
| `include/hotstuff/entity.h` | Block class with `order_qc` field, `ordering_map` storage |
| `include/hotstuff/crypto.h` | PartCert/QuorumCert (sign, verify, aggregate, clone) |
| `src/consensus.cpp` | CSC core logic: `on_propose`, `on_tx_sync`, `on_sort_vote`, `finalize_pending_proposal`, `on_receive_proposal`, `exec_blk`, commit callback |
| `src/hotstuff.cpp` | Network handlers: `new_view_handler`, `tx_sync_handler`, `sort_vote_handler`; `do_send_*` implementations |

## Detailed Code Flow

### Leader: `on_propose()` → `do_send_tx_sync()`
1. Creates new block with **empty ordering** (line 298-304)
2. Sets `pending_proposal = bnew`, `view_to_block[view] = bnew_hash`
3. Broadcasts TxSync to all replicas (line 321): carries leader block hash + current tx set
4. Blocks waiting for Sort-Votes (leader pauses on `pending_proposal`)

### Replica: `on_tx_sync()` → Sort-Vote
1. Builds `tx_set` from received `cur_txs` (line 190: O(S))
2. Calls `get_local_ordering(tx_set)` (line 191: O(R))
3. Censorship detection (optional, lines 194-204): iterates `receive_order_log` checking for missing txns
4. Sends Sort-Vote to leader (line 209)

### Leader: `on_sort_vote()` → aggregation
1. Collects Sort-Votes into `pending_sort_votes[view]` (line 218)
2. Censorship tracking (lines 223-231): counts missing txn reports across votes
3. When `votes.size() >= need` (n-f = n_majority): stores raw votes as block's ordering_map (lines 238-248)
4. Triggers `finalize_pending_proposal()` (line 250-252)

### Leader: `finalize_pending_proposal()` → OrderQC formation
1. Calls `WeightOrderer::order(votes, weight_k)` with N local orderings (line 261)
2. Stores final ordering as single-entry `ordering_map` on block (lines 262-266)
3. Creates OrderQC = `create_quorum_cert(block_hash)` (line 268)
4. Calls `do_consensus(block)` which broadcasts the proposal (line 272)

### Replica: `on_receive_proposal()` → OrderQC verification
1. Copies ordering_map from proposal to block (lines 333-334)
2. OrderQC verification (lines 335-347): checks `oqc->get_obj_hash() == block_hash` and `oqc->verify(config)`
3. Standard HotStuff safety/liveness check (lines 349-369): determines whether to vote
4. If opinion=true → votes (line 374-377)

### Commit Path (from block commit callback)
For each committed block between `b_exec` and new block:
1. `cleanup()` — erases view state, missing_vote_count, resets timeout params
2. `do_consensus(blk)` — standard HotStuff QC chain update
3. If block has non-empty `ordering_map` (CSC block):
   - Uses `ordering_map[0]` directly — the leader already finalized the ordering in `finalize_pending_proposal()` via `WeightOrderer::order(votes, k)`. Re-running WeightOrderer would double-apply `final_order_phase` (recursive median split), producing an incorrect result. (Fix #7)
4. Else (non-CSC / fallback):
   - Calls `get_local_ordering(cmd_set)` — iterates over `receive_order_log`
   - Then WeightOrderer on the local ordering (sole ordering authority for this path)
5. `do_decide()` for each command in the ordered result — delivers to application

### WeightOrderer Algorithm (`auncel.h`)

```
order(local_orderings, k):
  1. weight_order_phase: For each ordering (N at leader, N=1 at commit)
     For each txn at position d:
       weight = 1 - k^d  (uses pow_cache for d<64, std::pow for d>=64)
     Weight accumulates across all orderings per txn
     Sort all txns by ascending weight
     → O(N×S) weight computation + O(S log S) sort

  2. final_order_phase: Recursive median-split reordering
     Take median element by position
     Recursively process left half and right half
     Return [pre_sorted..., median, post_sorted...]
     → O(S log S) recursion with O(S) vector copies per level
```

## CSC Implementation Details

### Phase 1: TxSync (leader → replicas)
- `on_propose()` → `do_send_tx_sync(view, cur_txs, leader_id, block_hash)`
- TxSync broadcast to all replicas with HighQC and current pending txs

### Phase 2: Sort-Vote (replicas → leader)
- `on_tx_sync()` → each replica computes local ordering via `get_local_ordering()`
- `do_send_sort_vote(leader, local_ordering, missing_txs, view, block_hash)`

### Phase 3: OrderQC Formation + Verification
- `on_sort_vote()`: leader aggregates Sort-Votes, when n-f received calls `WeightOrderer::order()` + `create_quorum_cert(block_hash)`
- `on_receive_proposal()` verifies OrderQC: checks `has_order_qc()`, `get_obj_hash() == block_hash`, `oqc->verify(config)`
- OrderQC serialized via Proposal presence flag (`uint32_t has_oqc`)

### VVC (View-Value Consensus)
- Triggered only on `PMRoundRobinProposer::rotate()` — NOT during normal operation
- `rotate()` → `on_view_change()` (resets VVC state, increments view_counter, new leader seeds itself)
- Non-leaders: `do_send_new_view(new_leader, view)` → MsgNewView with pending txs + signed hqc cert
- Leader: `new_view_handler()` verifies cert, collects responses, sets `vvc_ready=true` at n-f
- `on_exp_timeout()`: leader blocks proposing until `vvc_ready`

### PMRoundRobinProposer Liveness
- `init()` → `stop_rotate()` → initial leader proposes directly (no timer, no VVC check)
- Normal operation: leader proposes via `beat()` from client commands, `do_new_consensus()` from QC completion
- Rotation cycle: `rotate()` → [exp_timeout timer] → `on_exp_timeout()` → [prop_delay timer] → `rotate()` → ...
- `on_consensus()` fires when block committed → `stop_rotate()` if from current proposer
- `stop_rotate()` re-proposes pending commands via `do_new_consensus(0, cmds)`
- **Must use `--pace-maker rr`** in config, NOT `dummy`

### BoxObj Pattern (quorum_cert_bt)
- `quorum_cert_bt` = `BoxObj<QuorumCert>` — non-copyable, move-only
- Cannot be a Proposal struct member — causes copy constructor deletion
- Solution: serialize QC through Proposal `serialize()/unserialize()` with presence flag
- Block stores `order_qc` as a `quorum_cert_bt` field directly (Block is heap-allocated via `block_t = RcObj<Block>`)

## Build

```bash
cd Auncel && mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .
make -j$(nproc)
```

Output: `build/examples/auncel-app`, `build/examples/auncel-client`

## Testing

### Unit Tests
```bash
cd build && ./test/test_auncel_integration
```
Tests: WeightOrderer logic, TxSync/NewView construction, Sort-Vote missing detection, f+1 threshold.

### Local Integration Test
```bash
cd Auncel && bash test_csc.sh    # 4 nodes on localhost, 15s duration
```