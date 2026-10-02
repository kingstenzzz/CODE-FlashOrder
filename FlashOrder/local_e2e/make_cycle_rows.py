#!/usr/bin/env python3
"""Generate a noisy hc20-style certificate (rows.txt) for the --rows-file injector.

n=800 txns, nrep=21 replicas, f=6 byzantine (shuffled), noise=5 (per-replica
poisson-ish adjacent swaps), cycle=20 window embedded at the batch middle.
Replicas 0..nrep-1: f random ones get a full shuffle (byzantine), the rest get
identity + noise swaps + a left-rotation of the 20-window by honest rank.
"""
import random
import sys

def generate(n, nrep, f, noise, seed, cycle_len):
    rng = random.Random(seed)
    rid = list(range(nrep)); rng.shuffle(rid)
    byz = [False] * nrep
    for i in range(f):
        byz[rid[i]] = True
    honest_rank = 0
    W = cycle_len if cycle_len > 0 else 0
    lo = (n - W) // 2
    rows = []
    for r in range(nrep):
        o = list(range(n))
        if byz[r]:
            rng.shuffle(o)
        else:
            for _ in range(rng.randint(0, noise * 2)):
                i = rng.randint(0, n - 2)
                o[i], o[i + 1] = o[i + 1], o[i]
            if W > 1:
                w = o[lo:lo + W]
                k = honest_rank % W
                w = w[k:] + w[:k]
                o[lo:lo + W] = w
            honest_rank += 1
        rows.append(o)
    return rows

if __name__ == "__main__":
    n, nrep, f, noise, seed, cycle = 800, 21, 6, 0, 4242, 20
    rows = generate(n, nrep, f, noise, seed, cycle)
    with open("rows.txt", "w") as fh:
        for row in rows:
            fh.write(" ".join(map(str, row)) + "\n")
    print(f"wrote rows.txt: {nrep} rows x {n} txns (f={f} byz, noise={noise}, cycle={cycle})")
