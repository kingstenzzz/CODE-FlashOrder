#!/usr/bin/env python3
"""Parse one e2e run's logs + verify replay, print one CSV line.

Usage: python3 analyze_run.py <arm> <seed>
Reads: replica_*.log, client.log, rows.txt (in cwd).
"""
import glob
import re
import subprocess
import sys


def read_all(pattern):
    out = ""
    for f in sorted(glob.glob(pattern)):
        try:
            out += open(f, errors="ignore").read()
        except OSError:
            pass
    return out


def main():
    arm, seed = sys.argv[1], sys.argv[2]
    logs = read_all("replica_*.log")
    cl = read_all("client.log")

    # ---- ordering telemetry (leader log) ----
    def grab(pattern, grp=1):
        m = re.search(pattern, logs)
        return m.group(grp) if m else "NA"

    d_t = grab(r"D_t=([\d.]+) K_D=")
    delta = grab(r"K_D=[\d.]+ delta=(\d+)")
    clusters = grab(r"detect_clusters_refined DONE, clusters=(\d+)")
    n_multi = grab(r"SCC stats: total=\d+, multi=(\d+), max_size=(\d+)", 1)
    max_scc = grab(r"SCC stats: total=\d+, multi=\d+, max_size=(\d+)")
    propose_ms = grab(r"flashorder_propose: txns=\d+ total_ms=([\d.]+)")

    # ---- client latency ----
    m = re.search(r"\[CLIENT_LAT\] n=\d+ p50_ms=([\d.]+) p95_ms=([\d.]+)", cl)
    p50, p95 = (m.group(1), m.group(2)) if m else ("NA", "NA")

    # ---- replay verification ----
    # ROWS_DBG: txn hashes in injection order (index = position)
    m = re.search(r"\[ROWS_DBG\] m=\d+ txs=([0-9a-f,]+)", cl)
    hash_by_idx = m.group(1).split(",") if m else []
    idx_by_hash = {h: i for i, h in enumerate(hash_by_idx)}

    # FINAL_ORDER_FULL: leader's final order (full hashes, 15/line)
    leader_hashes = []
    m = re.search(r"\[FINAL_ORDER_FULL\][\s\S]*", logs)
    for p in re.findall(r"\[FINAL_ORDER_FULL\] part=\d+/\d+ ([0-9a-f,]+)", logs):
        leader_hashes.extend(p.split(","))
    e2e_order = [idx_by_hash[h] for h in leader_hashes if h in idx_by_hash]

    replay_order = None
    if hash_by_idx:
        out = subprocess.run(
            ["../replay_certificate", "rows.txt", "21", "6", "0.8", "0.0"],
            capture_output=True, text=True).stdout
        for line in out.splitlines():
            if line.startswith("ORDER:"):
                replay_order = [int(x) for x in line.split()[1:]]
    replay_match = "NA"
    if e2e_order and replay_order:
        replay_match = str(e2e_order == replay_order)

    print(f"{arm},{seed},{d_t},{delta},{clusters},{n_multi},{max_scc},"
          f"{propose_ms},{p50},{p95},{replay_match}")


if __name__ == "__main__":
    main()
