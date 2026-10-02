# Auncel CSC Benchmark Results

Hardware: CloudLab, 4 server nodes + 1 client
- Servers: 128.110.219.142, .129, .140, .117
- Client: 128.110.219.139
- Shared storage: /proj/a2234-PG0/auncel/

Parameters: block_size=200, max_async=1000, duration=30s, pace-maker=rr, SmallBank (sb_users=100000, prob=0.9, skew=0.5)

## CSC v2 (Fix 1-4, no VVC)

Fixes applied:
1. Sort-Vote leader routing (TxSync carries actual leader_id + block_hash)
2. OrderQC cryptographic binding (sign block hash, not zero)
3. OrderQC verification on receipt (on_receive_proposal checks has_order_qc + verify)
4. Leader pre-computes final ordering (finalize_pending_proposal calls WeightOrderer::order)

| Nodes | TPS    | Lat avg (ms) | Lat p95 (ms) | Total txs |
|-------|--------|-------------|-------------|-----------|
| 9     | 25,417 | 39          | -           | -         |
| 21    | 16,134 | 62          | -           | -         |
| 41    | 13,301 | 75          | -           | -         |
| 61    | 9,604  | 104         | -           | -         |
| 81    | 8,064  | 124         | -           | -         |
| 101   | 6,655  | 151         | -           | -         |

### v2 vs FlashOrder

| Nodes | Auncel TPS | FlashOrder TPS | Auncel/FlashOrder |
|-------|-----------|---------------|-------------------|
| 9     | 25,417    | ~29,900       | 85%               |
| 21    | 16,134    | ~25,200       | 64%               |
| 41    | 13,301    | ~14,300       | 93%               |
| 61    | 9,604     | ~10,700       | 90%               |
| 81    | 8,064     | ~9,400        | 86%               |
| 101   | 6,655     | ~8,200        | 81%               |

FlashOrder leads at all node counts. Paper narrative preserved.
