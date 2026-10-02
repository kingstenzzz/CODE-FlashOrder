#!/usr/bin/env python3
"""Diagnose C++ vs Python PD replay divergence for one envelope run dir."""
import os, re, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PD = os.path.abspath(os.path.join(HERE, "..", "..", "..", "data-r1",
                                  "28_hc20_macro_cycle_stress", "script"))
sys.path.insert(0, PD)
from pd_core import PDSorter, preference_matrix_np


def load(rundir):
    leader = None
    for i in range(21):
        p = os.path.join(rundir, f"rep{i}.log")
        if not os.path.exists(p):
            continue
        t = open(p, "rb").read().decode(errors="ignore")
        if "[FINAL_ORDER_FULL]" in t:
            leader, text = i, t
            break
    fp = {int(p): hs.split(",") for p, _, hs in
          re.findall(r"\[FINAL_ORDER_FULL\] part=(\d+)/(\d+) (\S+)", text)}
    fin = [h for p in sorted(fp) for h in fp[p]]
    rows = {}
    for f, p, _, hs in re.findall(r"\[ROW_DBG\] from=(\d+) part=(\d+)/(\d+) (\S+)", text):
        rows.setdefault(int(f), {})[int(p)] = hs.split(",")
    sr = re.search(r"\[SORT_ROWS\] n=(\d+) rows=(\S+)", text)
    used, order_ids = set(), []
    if sr and sr.group(2):
        for x in sr.group(2).split(","):
            rid = int(x.split(":")[0])
            used.add(rid); order_ids.append(rid)
    if used:
        rows = {f: v for f, v in rows.items() if f in used}
    R = {f: [h for p in sorted(v) for h in v[p]] for f, v in rows.items()}
    ct = open(os.path.join(rundir, "client.log"), "rb").read().decode(errors="ignore")
    sp = {int(p): hs.split(",") for p, _, hs in
          re.findall(r"\[ENVELOPE_DBG\] spine part=(\d+)/(\d+) (\S+)", ct)}
    tx = [h for p in sorted(sp) for h in sp[p]]
    gp = {}
    for g, sz, p, _, hs in re.findall(
            r"\[ENVELOPE_DBG\] group=(\d+) size=(\d+) part=(\d+)/(\d+) (\S+)", ct):
        gp.setdefault(int(g), {})[int(p)] = hs.split(",")
    for g in sorted(gp):
        tx += [h for p in sorted(gp[g]) for h in gp[g][p]]
    load.order_ids = order_ids
    return leader, tx, R, fin


def main(rundir):
    leader, tx, rows, fin = load(rundir)
    # C++ assigns tx ids by first appearance scanning the collected rows
    order_ids = getattr(load, "order_ids", None)
    cpp_id = {}
    for rid in (order_ids or list(rows)):
        for h in rows[rid]:
            if h not in cpp_id:
                cpp_id[h] = len(cpp_id)
    idx = {h: i for i, h in enumerate(tx)}
    ri = [[cpp_id[h] for h in rows[rid]] for rid in (order_ids or list(rows))]
    inv = {v: k for k, v in cpp_id.items()}
    rep = PDSorter(21, 7, gamma=1.0, k_d=1.0).sort(ri)
    py = [inv[t] for t in rep["order"]]
    print(f"leader={leader} rows={len(ri)} rowsets_equal={len({frozenset(r) for r in ri})==1}")
    print(f"cpp_len={len(fin)} py_len={len(py)} same_set={set(fin)==set(py)}")
    d = [i for i, (a, b) in enumerate(zip(fin, py)) if a != b]
    print("first_diff:", d[0] if d else None, "n_diff:", len(d))
    pref = preference_matrix_np(np.array(ri))
    pos_cpp = {h: i for i, h in enumerate(fin)}
    pos_py = {h: i for i, h in enumerate(py)}
    if d:
        i = d[0]
        for name, order in (("cpp", fin), ("py", py)):
            win = order[i:i + 6]
            sc = [int((pref[idx[h]] - pref[:, idx[h]]).sum()) for h in win]
            print(f"  {name}[{i}:{i+6}] scores={sc}")
        # check the swapped pair
        a, b = fin[i], py[i]
        ia, ib = idx[a], idx[b]
        print(f"  cpp[i]={a[:8]} py[i]={b[:8]} "
              f"cpp_pos_of_py_i={pos_cpp[b]} py_pos_of_cpp_i={pos_py[a]}")
        print(f"  net scores: cpp_i={int((pref[ia]-pref[:,ia]).sum())} "
              f"py_i={int((pref[ib]-pref[:,ib]).sum())}")
    print("clusters:", [len(g) for g in sorted(rep["clusters"], key=len, reverse=True)][:5],
          "delta", rep["delta"], "threshold", rep["threshold"])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else
         os.path.join(HERE, "m200_cycle_s0"))
