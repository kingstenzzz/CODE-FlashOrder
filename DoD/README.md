# DoD-HS: DoD ordering layer on ordinary HotStuff

This variant adapts the DoD ordering layer to the repository's ordinary
HotStuff chain.  It is an experimental comparison implementation, not a
Narwhal/Tusk reproduction.

## Protocol boundary

- Each replica constructs a local dependency DAG from its receive order.
- The current leader collects `n-f` authenticated `LOCAL-ORDER` messages.
- The leader computes DoD's fixed/pending thresholds, weighted pairwise edges,
  SCC condensation, pending pruning, and missing-edge carry-over.
- The resulting graph is proposed as HotStuff block data and becomes committed
  only through the normal HotStuff QC/three-chain path.
- The BFT fault bound is the ordinary HotStuff value
  `f=floor((n-1)/3)`; DoD's fairness parameter (`gamma`) only controls the
  fixed/pending ordering thresholds.

SCC members are represented as a batch by removing internal SCC edges.  Their
relative execution order is therefore unspecified, matching DoD's
batch-order-fairness boundary.  This implementation does not include DoD's
Narwhal workers, sharded mempool, availability certificates, or Tusk DAG.

## Build and unit test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=ON
cmake --build build --target test_dod_global_order -j2
ctest --test-dir build -R dod_global_order --output-on-failure
```

The existing `dod-hs-app` and `dod-hs-client` targets use the same local
deployment configuration shape as the other HotStuff variants.

## Evidence boundary

The unit tests validate the DoD ordering layer and cache-trigger behavior.
Successful compilation of the app/client/keygen targets validates the ordinary
HotStuff integration path at build level.  Narwhal workers, availability
certificates, Tusk ordering, multi-replica deployment throughput, and
end-to-end latency are not implemented or claimed here.  The existing
transport-level LocalOrder peer-identity check is retained, but LocalOrder is
not yet bound to a signed view/parent identifier; adversarial replay and
leader-change validation therefore remain follow-up work.
