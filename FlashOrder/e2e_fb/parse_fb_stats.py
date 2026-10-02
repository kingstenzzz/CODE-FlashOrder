#!/usr/bin/env python3
"""parse_fb_stats.py -- 从 9 副本日志提取 fallback 触发率 / D_t / 排序耗时"""
import re, sys, glob, os

d = sys.argv[1]
scc_re = re.compile(r"SCC stats: total=(\d+), multi=(\d+), max_size=(\d+)")
dt_re = re.compile(r"D_t=([\d.]+) K_D=([\d.]+) delta=(\d+)")
perf_re = re.compile(r"\[perf\] flashorder_propose: txns=(\d+) total_ms=([\d.]+)")
thr_re = re.compile(r"(\d+) tps")

tot = {"blocks": 0, "fb": 0, "maxs": 0, "multi": 0}
dts, mss = [], []
tpss = []
for f in sorted(glob.glob(os.path.join(d, "rep*.log"))):
    for line in open(f, errors="ignore"):
        m = scc_re.search(line)
        if m:
            tot["blocks"] += 1
            t, multi, mx = map(int, m.groups())
            if multi > 0:
                tot["fb"] += 1
                tot["maxs"] = max(tot["maxs"], mx)
                tot["multi"] += multi
                mss.append(mx)
            continue
        m = dt_re.search(line)
        if m:
            dts.append(float(m.group(1)))
        m = perf_re.search(line)
        if m and float(m.group(2)) > 0:
            pass
for line in open(os.path.join(d, "client.log"), errors="ignore"):
    for m in thr_re.finditer(line):
        tpss.append(int(m.group(1)))

n = max(tot["blocks"], 1)
print(f"dir={d}")
print(f"blocks(with SCC line)={tot['blocks']} fallback_blocks={tot['fb']} "
      f"fallback_rate={tot['fb']/n:.3f} max_scc_seen={tot['maxs']} mean_multi_when_fb="
      f"{tot['multi']/max(tot['fb'],1):.2f}")
if dts:
    dts.sort()
    print(f"D_t: n={len(dts)} p50={dts[len(dts)//2]:.4f} p95={dts[int(len(dts)*0.95)]:.4f} max={dts[-1]:.4f}")
if tpss:
    print(f"tps_samples(last5)={tpss[-5:]}")
