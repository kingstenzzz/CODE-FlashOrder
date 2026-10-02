#!/usr/bin/env python3
"""Generate certificate rows for the local 21-node campaign.

--arm control  : all 21 replicas receive the identity order (no attack).
--arm cycle    : clean 20-window rotation + f byzantine shuffles (noise=0),
                 which is the construction that actually triggers merge_scc.

Usage:
  python3 make_rows.py --arm control --seed 0 --out rows_control_s0.txt
  python3 make_rows.py --arm cycle --cycle 20 --seed 0 --out rows_cycle_s0.txt
"""
import argparse
import random


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", choices=["control", "cycle"], default="cycle")
    ap.add_argument("--n", type=int, default=800)
    ap.add_argument("--nrep", type=int, default=21)
    ap.add_argument("--f", type=int, default=6)
    ap.add_argument("--noise", type=int, default=0)
    ap.add_argument("--cycle", type=int, default=20)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    n, nrep = args.n, args.nrep
    if args.arm == "control":
        rows = [list(range(n)) for _ in range(nrep)]
    else:
        rows = generate(n, nrep, args.f, args.noise, args.seed, args.cycle)

    with open(args.out, "w") as fh:
        for row in rows:
            fh.write(" ".join(map(str, row)) + "\n")
    print(f"wrote {args.out}: arm={args.arm} n={n} nrep={nrep} f={args.f} "
          f"noise={args.noise} cycle={args.cycle} seed={args.seed}")


if __name__ == "__main__":
    main()
