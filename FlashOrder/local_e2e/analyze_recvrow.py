#!/usr/bin/env python3
"""Gate-B replay verification using the ACTUAL receive rows ([RECV_ROW]).

Reads replica_*.log (leader) for [RECV_ROW] (21 actual rows) and
[FINAL_ORDER_FULL] (final order), builds a canonical hash->index map, writes
the rows as indices, runs replay_certificate, and compares.

Usage: python3 analyze_recvrow.py <arm> <seed>
"""
import glob
import re
import subprocess
import sys


def read_logs():
    out = ""
    for f in sorted(glob.glob("replica_*.log")):
        try:
            out += open(f, errors="ignore").read()
        except OSError:
            pass
    return out


def main():
    arm, seed = sys.argv[1], sys.argv[2]
    logs = read_logs()
    cl = open("client.log", errors="ignore").read()

    norm = lambda h: h.zfill(64)

    # ---- RECV_ROW: reconstruct 21 actual receive rows ----
    rows = {}
    for m in re.finditer(r"\[RECV_ROW\] R=(\d+) n=\d+ part=\d+/\d+ ([0-9a-f,]+)", logs):
        r = int(m.group(1))
        rows.setdefault(r, []).extend(m.group(2).split(","))
    n_rows = len(rows)
    if n_rows == 0:
        print(f"{arm},{seed},RECV_ROW 缺失,,,,,,,,,NA")
        return
    row_ids = sorted(rows.keys())
    ordered_rows = [rows[r] for r in row_ids]
    m_txns = len(ordered_rows[0])

    # ---- FINAL_ORDER_FULL: final order (hashes) ----
    final_hashes = []
    for m in re.finditer(r"\[FINAL_ORDER_FULL\] part=\d+/\d+ ([0-9a-f,]+)", logs):
        final_hashes.extend(m.group(1).split(","))

    # ---- canonical hash -> index (sorted unique) ----
    all_hashes = sorted({norm(h) for row in ordered_rows for h in row})
    idx_by_hash = {h: i for i, h in enumerate(all_hashes)}

    # ---- write rows as indices ----
    with open("rows_replay.txt", "w") as fh:
        for row in ordered_rows:
            fh.write(" ".join(str(idx_by_hash[norm(h)]) for h in row) + "\n")

    # ---- replay ----
    out = subprocess.run(
        ["../replay_certificate", "rows_replay.txt", str(n_rows), "6", "0.8", "0.0"],
        capture_output=True, text=True).stdout
    replay_order = None
    for line in out.splitlines():
        if line.startswith("ORDER:"):
            replay_order = [int(x) for x in line.split()[1:]]

    # ---- map final order to indices, compare ----
    e2e_order = [idx_by_hash[norm(h)] for h in final_hashes if norm(h) in idx_by_hash]
    match = "NA"
    if e2e_order and replay_order:
        match = str(e2e_order == replay_order)

    # ---- telemetry ----
    def grab(pat, g=1):
        m = re.search(pat, logs)
        return m.group(g) if m else "NA"
    d_t = grab(r"D_t=([\d.]+) K_D=")
    clusters = grab(r"detect_clusters_refined DONE, clusters=(\d+)")
    n_multi = grab(r"SCC stats: total=\d+, multi=(\d+), max_size=\d+")
    max_scc = grab(r"SCC stats: total=\d+, multi=\d+, max_size=(\d+)")
    m2 = re.search(r"\[CLIENT_LAT\] n=\d+ p50_ms=([\d.]+) p95_ms=([\d.]+)", cl)
    p50, p95 = (m2.group(1), m2.group(2)) if m2 else ("NA", "NA")

    print(f"{arm},{seed},{d_t},{clusters},{n_multi},{max_scc},{p50},{p95},"
          f"{n_rows},{m_txns},{match}")


if __name__ == "__main__":
    main()
