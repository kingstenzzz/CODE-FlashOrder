#!/usr/bin/env python3
"""Offline re-verification of C++ vs Python PD hash equality for stored runs.

Replays each run dir's captured rows (the exact subset/order the C++ sort
consumed) through pd_core with C++-compatible tx-id relabeling and rewrites
the cpp_py_hash_match / hash_cpp / hash_py columns of envelope_results.csv.
No cluster re-run needed.
"""
import csv, glob, hashlib, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from check_replay import load
sys.path.insert(0, os.path.abspath(os.path.join(
    HERE, "..", "..", "..", "data-r1", "28_hc20_macro_cycle_stress", "script")))
from pd_core import PDSorter


def sha(order):
    return hashlib.sha256(",".join(order).encode()).hexdigest()


def verify(rundir):
    leader, tx, rows, fin = load(rundir)
    order_ids = getattr(load, "order_ids", None) or list(rows)
    cpp_id, inv = {}, {}
    for rid in order_ids:
        for h in rows[rid]:
            if h not in cpp_id:
                cpp_id[h] = len(cpp_id)
                inv[cpp_id[h]] = h
    ri = [[cpp_id[h] for h in rows[rid]] for rid in order_ids]
    rep = PDSorter(21, 7, gamma=1.0, k_d=1.0).sort(ri)
    py = [inv[t] for t in rep["order"]]
    return sha(fin) == sha(py), sha(fin), sha(py), sorted(len(g) for g in rep["clusters"])


def main():
    csv_path = os.path.join(HERE, "envelope_results.csv")
    rows = list(csv.DictReader(open(csv_path)))
    for r in rows:
        d = os.path.join(HERE, f"m{r['m']}_{r['arm']}_s{r['seed']}")
        ok, hc, hp, sizes = verify(d)
        r["cpp_py_hash_match"], r["hash_cpp"], r["hash_py"] = int(ok), hc[:16], hp[:16]
        r["final_clusters"] = len(sizes)
        print(f"m={r['m']:>3} {r['arm']:<7} s{r['seed']} match={int(ok)} "
              f"clusters={len(sizes)} top={sizes[-2:] if len(sizes) > 1 else sizes}")
    with open(csv_path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print("updated", csv_path)


if __name__ == "__main__":
    main()
