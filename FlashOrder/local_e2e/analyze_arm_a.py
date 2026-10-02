#!/usr/bin/env python3
"""Parse Arm A (control) benchmark: TPS/latency from client.log + structural
telemetry from the leader log.  Prints one CSV line.

Usage: python3 analyze_arm_a.py <run_id>
"""
import glob
import re
import sys
from datetime import datetime


def read_logs():
    out = ""
    for f in sorted(glob.glob("replica_*.log")):
        try:
            out += open(f, errors="ignore").read()
        except OSError:
            pass
    return out


def main():
    run_id = sys.argv[1]
    cl = open("client.log", errors="ignore").read()
    logs = read_logs()

    # ---- client latency / TPS ----
    pat = re.compile(r"^(\d{4}-\d{2}-\d{2} \S+) \[hotstuff info\] ([\d.]+)$")
    times, lats = [], []
    for line in cl.splitlines():
        m = pat.match(line.strip())
        if m:
            try:
                dt = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
                times.append(dt)
                lats.append(float(m.group(2)) * 1e3)  # ms
            except ValueError:
                pass
    n_tx = len(lats)
    tps = span = 0.0
    if n_tx >= 2:
        times.sort()
        span = (times[-1] - times[0]).total_seconds()
        tps = (n_tx - 1) / span if span > 0 else 0.0
    lats_sorted = sorted(lats)
    p50 = lats_sorted[n_tx // 2] if n_tx else 0.0
    p95 = lats_sorted[int(0.95 * (n_tx - 1))] if n_tx else 0.0

    # ---- structural telemetry (leader log, aggregated over blocks) ----
    def all_matches(pat):
        return [m for m in re.finditer(pat, logs)]

    dts = [float(m.group(1)) for m in all_matches(r"D_t=([\d.]+) K_D=")]
    clu = [int(m.group(1)) for m in all_matches(r"detect_clusters_refined DONE, clusters=(\d+)")]
    scc = [(int(m.group(1)), int(m.group(2))) for m in
           all_matches(r"SCC stats: total=\d+, multi=(\d+), max_size=(\d+)")]
    prop = [float(m.group(1)) for m in all_matches(r"flashorder_propose: txns=\d+ total_ms=([\d.]+)")]

    n_blocks = len(dts)
    d_t_mean = sum(dts) / len(dts) if dts else 0.0
    d_t_max = max(dts) if dts else 0.0
    clu_mean = sum(clu) / len(clu) if clu else 0.0
    multi_blocks = sum(1 for m, _ in scc if m > 0)
    max_scc = max((s for _, s in scc), default=0)
    prop_mean = sum(prop) / len(prop) if prop else 0.0

    print(f"{run_id},{n_tx},{tps:.2f},{span:.2f},{p50:.2f},{p95:.2f},"
          f"{n_blocks},{d_t_mean:.5f},{d_t_max:.5f},{clu_mean:.1f},"
          f"{multi_blocks},{max_scc},{prop_mean:.2f}")


if __name__ == "__main__":
    main()
