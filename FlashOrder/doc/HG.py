"""Standalone FlashOrder preference-disagreement ordering core.

This module intentionally has no import from the recovered HG.py implementation.
It accepts a certificate as rows of transaction permutations and implements the
logical-delta path: preference matrix -> canonical positions -> clusters ->
quotient hypergraph -> deterministic topological order.
"""

from __future__ import annotations

import sys
from collections import deque
from dataclasses import dataclass
from math import ceil, floor, isfinite
from typing import Iterable, Sequence


@dataclass(frozen=True)
class OrderingResult:
    order: list[int]
    clusters: list[list[int]]
    preference_disagreement: float
    delta: int
    threshold: int
    n_multi_sccs: int = 0
    max_scc_size: int = 0


def validate_rows(rows: Sequence[Sequence[int]]) -> tuple[list[list[int]], int]:
    data = [list(map(int, row)) for row in rows]
    if not data:
        return [], 0
    m = len(data[0])
    expected = set(range(m))
    if any(len(row) != m or set(row) != expected for row in data):
        raise ValueError("certificate rows must be permutations of 0..m-1")
    return data, m


def preference_matrix(rows: Sequence[Sequence[int]]) -> list[list[int]]:
    data, m = validate_rows(rows)
    result = [[0] * m for _ in range(m)]
    for row in data:
        for left in range(m):
            first = row[left]
            for right in range(left + 1, m):
                result[first][row[right]] += 1
    return result


def disagreement(pref: Sequence[Sequence[int]]) -> float:
    m = len(pref)
    if any(len(row) != m for row in pref):
        raise ValueError("preference matrix must be square")
    numerator = denominator = 0
    for i in range(m):
        if pref[i][i] != 0:
            raise ValueError("preference diagonal must be zero")
        for j in range(i + 1, m):
            forward, reverse = int(pref[i][j]), int(pref[j][i])
            if forward < 0 or reverse < 0:
                raise ValueError("preference counts must be non-negative")
            numerator += 2 * min(forward, reverse)
            denominator += forward + reverse
    return numerator / denominator if denominator else 0.0


def logical_delta(pref: Sequence[Sequence[int]], k_d: float = 1.0) -> int:
    if not isfinite(float(k_d)) or float(k_d) < 0:
        raise ValueError("k_d must be finite and non-negative")
    m = len(pref)
    if m == 0:
        return 0
    return max(1, min(max(1, m - 1), 1 + ceil(float(k_d) * disagreement(pref))))


class PreferenceDisagreementSorter:
    """Deterministic, standalone logical-delta FlashOrder reference core."""

    def __init__(self, n_nodes: int, f_byzantine: int, gamma: float = 1.0, k_d: float = 1.0, margin: float = 1.2,
                 scc_nps_mode: str = "faithful"):
        if n_nodes <= 0 or not 0 <= f_byzantine < n_nodes:
            raise ValueError("invalid node/fault configuration")
        if not 0 <= float(gamma) <= 1 or not isfinite(float(gamma)):
            raise ValueError("gamma must be in [0, 1]")
        if float(margin) <= 1:
            raise ValueError("margin must be greater than one")
        self.n_nodes = int(n_nodes)
        self.f_byzantine = int(f_byzantine)
        self.gamma = float(gamma)
        self.k_d = float(k_d)
        self.margin = float(margin)
        self.scc_nps_mode = scc_nps_mode
        self.threshold = max(1, floor(self.n_nodes * (1 - self.gamma)) + self.f_byzantine + 1)

    def canonical_positions(self, pref: Sequence[Sequence[int]]) -> list[int]:
        return [sum(1 for other in range(len(pref)) if other != tx and pref[other][tx] >= self.threshold) for tx in range(len(pref))]

    def clusters(self, pref: Sequence[Sequence[int]]) -> tuple[list[list[int]], int]:
        positions = self.canonical_positions(pref)
        delta = logical_delta(pref, self.k_d)
        ordered = sorted(range(len(positions)), key=lambda tx: (positions[tx], tx))
        if not ordered:
            return [], 0
        clusters = [[ordered[0]]]
        for previous, current in zip(ordered, ordered[1:]):
            if positions[current] - positions[previous] < delta:
                clusters[-1].append(current)
            else:
                clusters.append([current])
        return clusters, delta

    def sort(self, rows: Sequence[Sequence[int]]) -> OrderingResult:
        data, m = validate_rows(rows)
        if not m:
            return OrderingResult([], [], 0.0, 0, self.threshold)
        pref = preference_matrix(data)
        clusters, delta = self.clusters(pref)
        cluster_of = {tx: index for index, group in enumerate(clusters) for tx in group}
        internal = []
        for group in clusters:
            internal.append(sorted(group, key=lambda tx: (-sum(pref[tx][other] - pref[other][tx] for other in group if other != tx), tx)))
        edges = [[] for _ in range(len(clusters))]
        indegree = [0] * len(clusters)
        for left in range(len(clusters)):
            for right in range(left + 1, len(clusters)):
                forward = sum(pref[a][b] for a in clusters[left] for b in clusters[right])
                reverse = sum(pref[b][a] for a in clusters[left] for b in clusters[right])
                if forward > self.margin * reverse:
                    edges[left].append(right)
                    indegree[right] += 1
                elif reverse > self.margin * forward:
                    edges[right].append(left)
                    indegree[left] += 1

        # Faithful port of C++ HyperGraph::topological_sort(): Tarjan SCC ->
        # condensation -> Kahn over the SCC DAG -> within-SCC cluster ordering.
        # Replaces the previous plain Kahn + residual-append, which did not
        # reproduce the deployed merge_scc path on cyclic quotient graphs.
        scc_id, scc_count = self._tarjan_scc(edges)
        scc_adj, scc_indeg = self._condense(edges, scc_id, scc_count)

        ready = deque(i for i in range(scc_count) if scc_indeg[i] == 0)
        group_seq = []
        while ready:
            u = ready.popleft()
            group_seq.append(u)
            for v in scc_adj[u]:
                scc_indeg[v] -= 1
                if scc_indeg[v] == 0:
                    ready.append(v)

        scc_members = [[] for _ in range(scc_count)]
        for i in range(len(clusters)):
            scc_members[scc_id[i]].append(i)
        n_multi_sccs = sum(1 for members in scc_members if len(members) > 1)
        max_scc_size = max((len(members) for members in scc_members), default=0)

        cluster_order = []
        for u in group_seq:
            cluster_order.extend(self._order_within_scc(scc_members[u], pref, clusters))
        order = [tx for cluster in cluster_order for tx in internal[cluster]]
        return OrderingResult(order, clusters, disagreement(pref), delta, self.threshold,
                              n_multi_sccs, max_scc_size)

    def _tarjan_scc(self, edges: Sequence[Sequence[int]]) -> tuple[list[int], int]:
        """Deterministic Tarjan; mirrors C++ tarjan_scc traversal/SCC numbering."""
        H = len(edges)
        if H == 0:
            return [], 0
        if H + 10 > sys.getrecursionlimit():
            sys.setrecursionlimit(H + 1000)
        index = [-1] * H
        lowlink = [-1] * H
        on_stack = [False] * H
        stack = []
        scc_id = [-1] * H
        timer = 0
        scc_count = 0

        def dfs(u: int) -> None:
            nonlocal timer, scc_count
            index[u] = lowlink[u] = timer
            timer += 1
            stack.append(u)
            on_stack[u] = True
            for v in edges[u]:
                if index[v] == -1:
                    dfs(v)
                    lowlink[u] = min(lowlink[u], lowlink[v])
                elif on_stack[v]:
                    lowlink[u] = min(lowlink[u], index[v])
            if lowlink[u] == index[u]:
                while True:
                    w = stack.pop()
                    on_stack[w] = False
                    scc_id[w] = scc_count
                    if w == u:
                        break
                scc_count += 1

        for i in range(H):
            if index[i] == -1:
                dfs(i)
        return scc_id, scc_count

    @staticmethod
    def _condense(edges: Sequence[Sequence[int]], scc_id: Sequence[int],
                  scc_count: int) -> tuple[list[list[int]], list[int]]:
        """Build the SCC condensation DAG (dedup edges, count indegree)."""
        pairs = [[] for _ in range(scc_count)]
        for u in range(len(edges)):
            for v in edges[u]:
                if scc_id[u] != scc_id[v]:
                    pairs[scc_id[u]].append(scc_id[v])
        scc_adj = []
        scc_indeg = [0] * scc_count
        for i in range(scc_count):
            p = sorted(set(pairs[i]))
            scc_adj.append(p)
            for v in p:
                scc_indeg[v] += 1
        return scc_adj, scc_indeg

    def _order_within_scc(self, members: Sequence[int], pref: Sequence[Sequence[int]],
                          clusters: Sequence[Sequence[int]]) -> list[int]:
        """Order clusters inside one SCC by net preference score.

        ``faithful`` (default) reproduces the deployed C++ ``order_within_scc``,
        which indexes ``pref`` by the *cluster index* rather than by member
        transactions (a known quirk in flashorder.h L327-350). ``correct`` sums
        the inter-cluster preference over all member transactions instead.
        """
        if len(members) <= 1:
            return list(members)
        scores = []
        for m in members:
            net = 0
            if self.scc_nps_mode == "correct":
                for o in members:
                    if o == m:
                        continue
                    forward = sum(pref[a][b] for a in clusters[m] for b in clusters[o])
                    reverse = sum(pref[b][a] for a in clusters[m] for b in clusters[o])
                    net += forward - reverse
            else:  # faithful: cluster index is used directly as a pref row/col
                for o in members:
                    if o != m:
                        net += pref[m][o] - pref[o][m]
            scores.append((net, m))
        scores.sort(key=lambda p: (-p[0], p[1]))
        return [m for _, m in scores]


__all__ = ["OrderingResult", "PreferenceDisagreementSorter", "disagreement", "logical_delta", "preference_matrix"]
