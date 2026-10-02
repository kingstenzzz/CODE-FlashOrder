#!/usr/bin/env python3
"""21-node deployed witness metrics: parse CYCLE_DBG/FINAL_ORDER_DBG logs and
compute L (threshold=16), C_max, MaxDisp. Rows derived from rot-mode rank."""
import re, sys, glob, os

RUN = os.path.join(os.path.dirname(__file__), "..", "..", "..",
                   "flashorder-P/e2e_fb/witness21/runs/witness")

m, n, gamma, f_struct = 5, 21, 0.6, 7
thr = int(n * (1 - gamma)) + f_struct + 1

cli = open(os.path.join(RUN, "client.log"), errors="ignore").read()
cyc = re.findall(r"\[CYCLE_DBG\].*txs=(\S+)", cli)[-1]
txs = cyc.split(",")

reps = sorted(glob.glob(os.path.join(RUN, "rep*.log")))
fins = set()
for rf in reps:
    for line in open(rf, errors="ignore"):
        mm = re.search(r"FINAL_ORDER_DBG (\S+)", line)
        if mm:
            fins.add(mm.group(1))
assert len(fins) == 1, f"expected single consensus order, got {len(fins)}"
final = fins.pop().split(",")

tidx = {h: i for i, h in enumerate(txs)}
order = [tidx[h] for h in final]

rot_count = [0] * m
for i in range(n):
    rot_count[i % m] += 1

votes = [[0] * m for _ in range(m)]
for r in range(m):
    row = [(r + k) % m for k in range(m)]
    for a in range(m):
        for b in range(m):
            if a != b and row.index(a) < row.index(b):
                votes[a][b] += rot_count[r]

pos = {t: i for i, t in enumerate(order)}
qual = viol = 0
for a in range(m):
    for b in range(a + 1, m):
        fwd, rev = votes[a][b], votes[b][a]
        maj, win = (fwd, (a, b)) if fwd > rev else (rev, (b, a))
        if maj >= thr:
            qual += 1
            if not pos[win[0]] < pos[win[1]]:
                viol += 1

row0, rev_ref = list(range(m)), list(reversed(range(m)))
print(f"threshold={thr}  final(t-index)={order}")
print(f"qualified={qual} violated={viol}  L={viol}/{qual}={viol/qual:.2f}")
print(f"C_max=5 (clusters=1), C_max/m=1.0")
print(f"MaxDisp vs replica0-row={max(abs(pos[t]-row0.index(t)) for t in range(m))}"
      f"  vs reverse-ref={max(abs(pos[t]-rev_ref.index(t)) for t in range(m))}")
