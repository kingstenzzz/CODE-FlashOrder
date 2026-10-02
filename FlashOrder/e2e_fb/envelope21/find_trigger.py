#!/usr/bin/env python3
"""Search for a minimal workload that TRIGGERS the strict-repair coverage gate.

Faithful Python port of flashorder.h strict path (default clustering -> SCC fold
-> coverage loop). Reports the first row-set where repair_rounds > 0 (a violated
admitted pair in distinct clusters with no forward quotient path).
"""
import itertools, sys
import numpy as np
sys.path.insert(0, "/home/kkk/code/new-code/Order-Fairness/data-r1/28_hc20_macro_cycle_stress/script")
from pd_core import preference_matrix_np, disagreement_np, logical_delta, tarjan_sccs


def default_clusters(pref, threshold, k_d):
    m = len(pref)
    p = pref.copy()
    np.fill_diagonal(p, 0)
    positions = (p >= threshold).sum(axis=0)
    delta = logical_delta(p, k_d)
    ordered = sorted(range(m), key=lambda t: (positions[t], t))
    clusters = [[ordered[0]]]
    for prev, cur in zip(ordered, ordered[1:]):
        if positions[cur] - positions[prev] < delta:
            clusters[-1].append(cur)
        else:
            clusters.append([cur])
    return clusters, delta, positions


def quotient_edges(clusters, pref, margin):
    K = len(clusters)
    member = np.zeros((K, len(pref)))
    for a, g in enumerate(clusters):
        member[a, g] = 1.0
    q = member @ pref.astype(float) @ member.T
    edges = {u: set() for u in range(K)}
    for a in range(K):
        for b in range(a + 1, K):
            if q[a, b] > margin * q[b, a]:
                edges[a].add(b)
            elif q[b, a] > margin * q[a, b]:
                edges[b].add(a)
    return edges


def closure(edges, K):
    R = [[False] * K for _ in range(K)]
    for u in range(K):
        for v in edges.get(u, ()):
            R[u][v] = True
    for k in range(K):
        for i in range(K):
            if R[i][k]:
                for j in range(K):
                    if R[k][j]:
                        R[i][j] = True
    return R


def strict_gate_merges(pref, clusters, threshold, margin=1.2):
    """Return (coverage_rounds, total_merges). Faithful to C++ flashorder.h."""
    m = len(pref)
    # initial SCC fold
    edges = quotient_edges(clusters, pref, margin)
    merges = 0
    # fold multi-node SCCs into single clusters (merge_clusters semantics)
    K = len(clusters)
    sccs = [c for c in tarjan_sccs(K, edges) if len(c) > 1]
    for comp in sccs:
        if len(comp) > 1:
            merges += len(comp) - 1
            # merge comp into one cluster
            new = []
            for t in comp:
                new.extend(clusters[t])
            clusters = [g for i, g in enumerate(clusters) if i not in comp] + [new]
            break  # simplify: fold one, then recompute below via loop
    # recompute edges after fold (loop handles repeated folding approximately)
    rounds = 0
    while True:
        rounds += 1
        K = len(clusters)
        if K <= 1:
            break
        tc = [-1] * m
        for ci, g in enumerate(clusters):
            for t in g:
                tc[t] = ci
        edges = quotient_edges(clusters, pref, margin)
        # fold any new SCC first
        sccs = [c for c in tarjan_sccs(K, edges) if len(c) > 1]
        if sccs:
            comp = sccs[0]
            new = []
            for t in comp:
                new.extend(clusters[t])
            clusters = [g for i, g in enumerate(clusters) if i not in comp] + [new]
            merges += len(comp) - 1
            continue
        R = closure(edges, K)
        # find a violating admitted pair
        found = None
        for a in range(m):
            for b in range(m):
                if a == b or pref[a][b] < threshold:
                    continue
                ca, cb = tc[a], tc[b]
                if ca == cb or ca < 0 or cb < 0:
                    continue
                if R[ca][cb]:
                    continue
                found = (a, b, ca, cb)
                break
            if found:
                break
        if found is None:
            break
        a, b, ca, cb = found
        new = clusters[ca] + clusters[cb]
        clusters = [g for i, g in enumerate(clusters) if i not in (ca, cb)] + [new]
        merges += 1
    return rounds, merges


def evaluate(rows, n_nodes, f, gamma, k_d=1.0, margin=1.2):
    pref = preference_matrix_np(np.asarray(rows)).astype(np.int64)
    thr = max(1, int(np.floor(n_nodes * (1 - gamma))) + f + 1)
    clusters, delta, pos = default_clusters(pref, thr, k_d)
    rounds, merges = strict_gate_merges(pref.copy(), [list(g) for g in clusters], thr, margin)
    return pref, thr, delta, clusters, rounds, merges


def main():
    n = 9
    f = 3
    gamma = 1.0
    rng = np.random.default_rng(0)
    best = None
    # random structured search: start from unanimous base + targeted swaps
    for trial in range(200000):
        m = int(rng.integers(4, 8))
        base = list(range(m))
        rows = []
        for r in range(n):
            perm = base.copy()
            # a few adjacent swaps + occasional block rotation to inject conflict
            nsw = int(rng.integers(1, 3))
            for _ in range(nsw):
                i = int(rng.integers(0, m - 1))
                perm[i], perm[i + 1] = perm[i + 1], perm[i]
            if rng.random() < 0.3:
                k = int(rng.integers(1, m))
                perm = perm[k:] + perm[:k]
            rows.append(perm)
        try:
            pref, thr, delta, clusters, rounds, merges = evaluate(rows, n, f, gamma)
        except Exception:
            continue
        if rounds > 1 and (best is None or m < best[0]):
            best = (m, rows, pref, thr, delta, clusters, rounds, merges)
            print(f"FOUND m={m} rounds={rounds} merges={merges} thr={thr} delta={delta} "
                  f"clusters={[len(g) for g in clusters]}")
            print("rows:")
            for r in rows:
                print("  ", r)
            if m <= 5:
                return
    if best is None:
        print("not found")
    else:
        m, rows, pref, thr, delta, clusters, rounds, merges = best
        print("BEST:", m, rounds, merges, [len(g) for g in clusters])


if __name__ == "__main__":
    main()
