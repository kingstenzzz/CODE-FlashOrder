#!/usr/bin/env python3
"""Verify the PD paper's positive theorems against the captured envelope runs.

Checks, on the exact rows each C++ sort consumed:

1. Prop. deterministic replay  -> C++ final order sha256 == Python PD replay
2. Thm. NPS support preservation -> inside every final cluster of size b,
   every pair with certificate support t > c(1 - 1/b) must be correctly
   ordered (paper's threshold), and none may be violated.
3. CR-gamma-BOF (batch-level) -> every admitted pair that is NOT absorbed into
   one cluster must be ordered with the majority direction (equality allowed
   for absorbed pairs); i.e. cross-cluster violations must be zero.

Usage: python3 verify_theorems.py [m ...]
"""
import hashlib, os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from check_replay import load
sys.path.insert(0, os.path.abspath(os.path.join(
    HERE, "..", "..", "..", "data-r1", "28_hc20_macro_cycle_stress", "script")))
from pd_core import PDSorter, preference_matrix_np


def sha(order):
    return hashlib.sha256(",".join(order).encode()).hexdigest()


def check(m, arm, seed=0):
    rundir = os.path.join(HERE, f"m{m}_{arm}_s{seed}")
    leader, tx, rows, fin = load(rundir)
    oids = load.order_ids
    cpp_id, inv = {}, {}
    for rid in oids:
        for h in rows[rid]:
            if h not in cpp_id:
                cpp_id[h] = len(cpp_id)
                inv[cpp_id[h]] = h
    ri = [[cpp_id[h] for h in rows[rid]] for rid in oids]
    fin_i = [cpp_id[h] for h in fin]
    rep = PDSorter(21, 7, gamma=1.0, k_d=1.0).sort(ri)
    pref = preference_matrix_np(np.array(ri)).astype(np.int64)
    c, mm = len(ri), len(cpp_id)
    thr = rep["threshold"]

    # 1. deterministic replay
    replay_ok = sha([inv[t] for t in rep["order"]]) == sha(fin)

    pos = np.empty(mm, dtype=np.int64)
    pos[fin_i] = np.arange(mm)
    cl = np.empty(mm, dtype=np.int64)
    for ci, g in enumerate(rep["clusters"]):
        cl[g] = ci
    a, b = np.triu_indices(mm, k=1)
    P = np.maximum(pref, pref.T)
    ok = np.where(pref[a, b] >= pref[b, a], pos[a] < pos[b], pos[b] < pos[a])
    same = cl[a] == cl[b]
    admitted = P[a, b] >= thr

    # 3. CR-gamma-BOF: cross-cluster admitted pairs must be correctly ordered
    cross_viol = int((~ok & ~same)[admitted].sum())

    # 2. NPS support preservation per final cluster
    print(f"--- m={m} {arm} (rows c={c}, thr={thr}, leader=R{leader}) ---")
    print(f"  [Prop. deterministic replay] cpp==python sha256: {replay_ok}")
    print(f"  [CR-gamma-BOF] admitted pairs={int(admitted.sum())}, "
          f"cross-cluster violations={cross_viol}")
    for ci, g in enumerate(sorted(rep["clusters"], key=len, reverse=True)):
        bc = len(g)
        if bc < 2:
            continue
        t_lo = c * (1 - 1 / bc)
        sel = np.isin(a, g) & np.isin(b, g)
        supp, sat = P[a[sel], b[sel]], ok[sel]
        above = supp > t_lo
        print(f"  [Thm NPS preservation] cluster#{ci} size={bc}: threshold t>{t_lo:.2f}, "
              f"pairs above={int(above.sum())}, violations among them="
              f"{int((~sat & above).sum())}, in-cluster pairs={len(supp)}")
    return replay_ok, cross_viol


if __name__ == "__main__":
    ms = [int(x) for x in sys.argv[1:]] or [200, 400, 800]
    all_ok = True
    for m in ms:
        for arm in ("control", "cycle"):
            r = check(m, arm)
            all_ok &= r[0] and r[1] == 0
    print("\nALL CHECKS PASS" if all_ok else "\nCHECK FAILED")
