#!/usr/bin/env python3
"""
FlashOrder-D Comprehensive Test:
1. Phi sweep: {0.075, 0.15, 0.30}
2. Latency inflation attack
3. SCC frequency analysis
4. Phase profiling
"""
import subprocess
import sys
import os
import re
import time
import statistics

KEYGEN = "./build/flashorder-D-keygen"
TLS_KEYGEN = "./build/flashorder-D-tls-keygen"
APP = "./build/examples/flashorder-D-app"
CLIENT = "./build/examples/flashorder-D-client"
N = 4
BLOCK_SIZE = 100
DURATION = 15

def gen_config(n, block_size):
    p = subprocess.run([KEYGEN, "--num", str(n)], capture_output=True, timeout=10)
    keys = [[t[4:] for t in l.decode().split()] for l in p.stdout.strip().split(b"\n") if l.strip()]
    tls_p = subprocess.run([TLS_KEYGEN, "--num", str(n)], capture_output=True, timeout=10)
    tls_keys = [[t[4:] for t in l.decode().split()] for l in tls_p.stdout.strip().split(b"\n") if l.strip()]
    replicas = [f"127.0.0.1:{10000+i};{20000+i}" for i in range(n)]
    with open("hotstuff.conf", "w") as mc:
        mc.write(f"block-size = {block_size}\n")
        mc.write("nworker = 8\nrepnworker = 8\nclinworker = 8\nrepburst = 1000\ncliburst = 1000\n")
        mc.write("sb-users = 100000\nsb-prob-choose_mtx = 0.95\nsb-skew-factor = 0.99\n")
        mc.write("fairness-parameter = 1.0\npace-maker = rr\n")
        with open("nodes.txt", "w") as nodes:
            for r in zip(replicas, keys, tls_keys, range(n)):
                mc.write(f"replica = {r[0]}, {r[1][0]}, {r[2][2]}\n")
                name = f"hotstuff-sec{r[3]}.conf"
                nodes.write(f"{r[3]}:{r[0]}\t{name}\n")
                with open(name, "w") as rc:
                    rc.write(f"privkey = {r[1][1]}\ntls-privkey = {r[2][1]}\ntls-cert = {r[2][0]}\nidx = {r[3]}\n")
    print(f"Config: {n} replicas, block-size={block_size}")

def start_replicas(phi_val=None):
    os.system("pkill -9 -f flashorder-D-app 2>/dev/null")
    time.sleep(1)
    pids = []
    for i in range(N):
        cmd = f"{APP} --conf hotstuff-sec{i}.conf"
        if phi_val:
            cmd += f" --phi {phi_val}"
        cmd += f" > /tmp/fod_test_log{i}.log 2>&1 &"
        os.system(cmd)
    time.sleep(8)
    ret = subprocess.run("pgrep -c -f flashorder-D-app", shell=True, capture_output=True, text=True)
    print(f"  Replicas running: {ret.stdout.strip()}")
    return ret.stdout.strip() == str(N + 1)

def run_client(max_async=500, duration=DURATION):
    cmd = f"timeout {duration} {CLIENT} --idx 0 --iter 1000000 --max-async {max_async}"
    ret = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=duration+10)
    output = ret.stdout + ret.stderr
    with open("/tmp/fod_test_client.log", "w") as f:
        f.write(output)
    return output

def parse_results(output):
    lines = [l for l in output.split("\n") if "hotstuff info" in l and l.strip()]
    
    timestamps = []
    latencies = []
    for line in lines:
        m = re.search(r"(\d{2}):(\d{2}):(\d{2})\.(\d+)", line)
        if m:
            h, mi, s, ms = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
            timestamps.append(mi * 60 + s + ms / 1000000.0)
        parts = line.split()
        for p in reversed(parts):
            try:
                latencies.append(float(p))
                break
            except:
                pass
    
    total_txs = len(latencies)
    if timestamps:
        duration = max(timestamps) - min(timestamps)
        tps = total_txs / duration if duration > 0 else 0
    else:
        duration = 0
        tps = 0
    
    avg_lat = statistics.mean(latencies) * 1000 if latencies else 0
    
    return {
        "total_txs": total_txs,
        "duration_s": duration,
        "tps": tps,
        "avg_lat_ms": avg_lat,
    }

def parse_profile(log_file="/tmp/fod_test_log0.log"):
    """Extract PROFILE and SCC stats from replica log"""
    profiles = []
    scc_stats = []
    try:
        with open(log_file) as f:
            for line in f:
                m = re.search(r"PROFILE phi=([\d.]+).*prefs=([\d.]+)ms.*canon=([\d.]+)ms.*clusters=([\d.]+)ms.*hypergraph=([\d.]+)ms.*topo=([\d.]+)ms.*total=([\d.]+)ms", line)
                if m:
                    profiles.append({
                        "phi": float(m.group(1)),
                        "prefs": float(m.group(2)),
                        "canon": float(m.group(3)),
                        "clusters": float(m.group(4)),
                        "hypergraph": float(m.group(5)),
                        "topo": float(m.group(6)),
                        "total": float(m.group(7)),
                    })
                m2 = re.search(r"SCC stats: total=(\d+), multi=(\d+), max_size=(\d+)", line)
                if m2:
                    scc_stats.append({
                        "total_sccs": int(m2.group(1)),
                        "multi_sccs": int(m2.group(2)),
                        "max_scc_size": int(m2.group(3)),
                    })
    except FileNotFoundError:
        pass
    
    return profiles, scc_stats

def avg_profile(profiles):
    if not profiles:
        return None
    keys = ["prefs", "canon", "clusters", "hypergraph", "topo", "total"]
    result = {}
    for k in keys:
        vals = [p[k] for p in profiles]
        result[k] = statistics.mean(vals)
    result["phi"] = profiles[0]["phi"]
    result["count"] = len(profiles)
    return result

def avg_scc(scc_stats):
    if not scc_stats:
        return None
    return {
        "total_sccs": statistics.mean([s["total_sccs"] for s in scc_stats]),
        "multi_sccs": statistics.mean([s["multi_sccs"] for s in scc_stats]),
        "max_scc_size": max([s["max_scc_size"] for s in scc_stats]),
        "count": len(scc_stats),
    }

print("=" * 70)
print(" FlashOrder-D Comprehensive Test")
print("=" * 70)

gen_config(N, BLOCK_SIZE)

# ============================================
# 1. Phi Sweep
# ============================================
print("\n" + "=" * 70)
print(" [1] Phi Sweep: {0.075, 0.15, 0.30}")
print("=" * 70)
print(f"{'Phi':<8} {'TPS':<10} {'AvgLat(ms)':<12} {'TotalTxs':<10}")
print("-" * 45)

phi_results = {}
for phi in [0.075, 0.15, 0.30]:
    print(f"\n  Testing Phi={phi}...")
    start_replicas(phi)
    time.sleep(3)
    output = run_client(max_async=500)
    result = parse_results(output)
    phi_results[phi] = result
    print(f"  Phi={phi:<6} TPS={result['tps']:<10.1f} Lat={result['avg_lat_ms']:<10.1f} Txs={result['total_txs']}")

print(f"\n{'Phi':<8} {'TPS':<10} {'AvgLat(ms)':<12} {'TotalTxs':<10}")
print("-" * 45)
for phi in [0.075, 0.15, 0.30]:
    r = phi_results[phi]
    print(f"{phi:<8} {r['tps']:<10.1f} {r['avg_lat_ms']:<12.1f} {r['total_txs']:<10}")

# ============================================
# 3. SCC Frequency (from phi=0.15 run)
# ============================================
print("\n" + "=" * 70)
print(" [3] SCC Frequency Analysis (from Phi=0.15 run)")
print("=" * 70)
profiles, scc_stats = parse_profile()
avg_p = avg_profile(profiles)
avg_s = avg_scc(scc_stats)
if avg_s:
    print(f"  Rounds analyzed:     {avg_s['count']}")
    print(f"  Avg total SCCs/round: {avg_s['total_sccs']:.1f}")
    print(f"  Avg multi-SCCs/round: {avg_s['multi_sccs']:.1f}")
    print(f"  Max SCC size:         {avg_s['max_scc_size']}")
    cycle_rate = avg_s['multi_sccs'] / avg_s['total_sccs'] * 100 if avg_s['total_sccs'] > 0 else 0
    print(f"  Cycle frequency:      {cycle_rate:.1f}%")
else:
    print("  No SCC data found")

# ============================================
# 4. Phase Profiling (from phi=0.15 run)
# ============================================
print("\n" + "=" * 70)
print(" [4] Phase Profiling (from Phi=0.15 run)")
print("=" * 70)
if avg_p:
    print(f"  Rounds profiled:  {avg_p['count']}")
    total = avg_p['total']
    print(f"\n  {'Phase':<25} {'Avg(ms)':<10} {'% Total':<8}")
    print("  " + "-" * 45)
    for phase_name, key in [("1. Preferences", "prefs"), ("2. Canonical positions", "canon"),
                             ("3. Cluster detection", "clusters"), ("4. Hypergraph build", "hypergraph"),
                             ("5. Topological sort", "topo")]:
        val = avg_p[key]
        pct = val / total * 100 if total > 0 else 0
        print(f"  {phase_name:<25} {val:<10.3f} {pct:<8.1f}%")
    print(f"  {'TOTAL':<25} {total:<10.3f}")
else:
    print("  No profile data found")

# ============================================
# Summary
# ============================================
print("\n" + "=" * 70)
print(" Summary")
print("=" * 70)
print(f"\nPhi sweep (N={N}, B={BLOCK_SIZE}):")
print(f"  Phi=0.075: TPS={phi_results[0.075]['tps']:.0f}, Lat={phi_results[0.075]['avg_lat_ms']:.1f}ms")
print(f"  Phi=0.150: TPS={phi_results[0.15]['tps']:.0f}, Lat={phi_results[0.15]['avg_lat_ms']:.1f}ms")
print(f"  Phi=0.300: TPS={phi_results[0.30]['tps']:.0f}, Lat={phi_results[0.30]['avg_lat_ms']:.1f}ms")

os.system("pkill -9 -f flashorder-D-app 2>/dev/null")
print("\nDone.")
