#!/usr/bin/env python3
"""21-node PD adversarial macro-cycle E2E envelope experiment (#29).

For m in {200,400,800} x seeds {0,1,2} x arms {cycle, control}:
  1. deploy 21 local replicas (BS=m, gamma=1.0, strict_repair=False) with
     FO_DBG_ROWS/FO_DBG_FULL
  2. evil-client macro mode: spine + 20 unequal groups; replica i sees group
     blocks rotated by i%20 (cycle arm) or rotation 0 (control arm)
  3. capture real 21xm observation rows (ROW_DBG) + C++ final order
     (FINAL_ORDER_FULL) from the actual leader
  4. offline PD replay (pd_core) on the exact captured rows; compare
     C++ final-order hash vs Python PD final-order hash
  5. metrics: L_gamma, RPS, C_max/m, D_max/(m-1), clusters, quotient
     compression, stage timings, client latency

Outputs: envelope_results.csv, envelope_metadata.json (in this directory).
"""
import csv, glob, hashlib, json, os, re, shutil, signal, statistics as st
import subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
E2E = os.path.dirname(HERE)                 # flashorder-P/e2e_fb
APP = os.path.join(E2E, "..", "build", "examples", "flashorder-P-app")
EVIL = os.path.join(E2E, "..", "build", "examples", "evil-client")
PD_CORE_DIR = os.path.abspath(os.path.join(
    HERE, "..", "..", "..", "data-r1", "28_hc20_macro_cycle_stress", "script"))
sys.path.insert(0, PD_CORE_DIR)
import numpy as np
from pd_core import PDSorter

N = 21
GAMMA = 1.0   # threshold=8 with f_struct=7; 0.6 would need 16 votes but only ~15 rows are cached at propose
L_GROUPS = 20
SEEDS = [0, 1, 2]
MS = [200, 400, 800]
KD = 1.0
STRICT = os.environ.get("STRICT", "0") == "1"   # FLASHORDER_P_STRICT_REPAIR


def group_layout(m):
    """v3 proportions: spine 210/800; w_i ~ 20+i. Exact for m=800."""
    if m == 800:
        return 210, [20 + i for i in range(L_GROUPS)]
    spine = int(round(m * 210 / 800))
    budget = m - spine
    raw = [20 + i for i in range(L_GROUPS)]
    tot = sum(raw)
    w = [max(2, budget * r // tot) for r in raw]
    w[-1] += budget - sum(w)               # absorb rounding in last group
    return spine, w


def sha_of(order_hexes):
    return hashlib.sha256(",".join(order_hexes).encode()).hexdigest()


def clean():
    subprocess.run(["pkill", "-9", "-x", "flashorder-P-ap"], capture_output=True)
    subprocess.run(["pkill", "-9", "-x", "evil-client"], capture_output=True)
    time.sleep(1.5)


def gen_conf(m, rundir):
    ips = os.path.join(rundir, "_ips.txt")
    with open(ips, "w") as fh:
        fh.write("127.0.0.1\n" * N)
    subprocess.run([sys.executable, os.path.join(E2E, "..", "scripts", "gen_conf.py"),
                    "--prefix", "hotstuff", "--ips", ips,
                    "--keygen", os.path.join(E2E, "..", "build", "flashorder-P-keygen"),
                    "--tls-keygen", os.path.join(E2E, "..", "build", "flashorder-P-tls-keygen"),
                    "--block-size", str(m), "--nworker", "2", "--repnworker", "2",
                    "--clinworker", "2", "--sb-users", "100000",
                    "--sb-prob-choose_mtx", "0.95", "--sb-skew-factor", "0.99",
                    "--fairness-parameter", str(GAMMA), "--pace-maker", "rr"],
                   cwd=rundir, check=True, capture_output=True)
    for f in glob.glob(os.path.join(rundir, "hotstuff-sec*.conf")):
        subprocess.run(["sed", "-i", "/^tls-privkey/d; /^tls-cert/d", f], check=True)


ROW_RE = re.compile(r"\[ROW_DBG\] from=(\d+) part=(\d+)/(\d+) (\S+)")
FIN_RE = re.compile(r"\[FINAL_ORDER_FULL\] part=(\d+)/(\d+) (\S+)")
ENV_SPINE_RE = re.compile(r"\[ENVELOPE_DBG\] spine part=(\d+)/(\d+) (\S+)")
ENV_GROUP_RE = re.compile(r"\[ENVELOPE_DBG\] group=(\d+) size=(\d+) part=(\d+)/(\d+) (\S+)")
PROF_RE = re.compile(r"PROFILE K_D=[\d.]+ prefs=([\d.]+)ms canon=([\d.]+)ms "
                     r"clusters=([\d.]+)ms hypergraph=([\d.]+)ms topo=([\d.]+)ms total=([\d.]+)ms")
PROP_RE = re.compile(r"\[perf\] flashorder_propose: txns=(\d+) total_ms=([\d.]+)")
CLUSTER_RE = re.compile(r"detect_clusters_refined DONE, clusters=(\d+)")
SORTROWS_RE = re.compile(r"\[SORT_ROWS\] n=(\d+) rows=(\S+)")
SCC_RE = re.compile(r"SCC stats: total=(\d+), multi=(\d+), max_size=(\d+)")
STRICT_RE = re.compile(r"strict_repair: rounds=(\d+) merges=(\d+) clusters=(\d+)")
CLI_LAT_RE = re.compile(r"\[CLIENT_LAT\] n=(\d+) p50_ms=([\d.]+) p95_ms=([\d.]+) avg_ms=([\d.]+)")


def run_one(m, seed, arm, rundir):
    logdir = os.path.join(rundir, f"m{m}_{arm}_s{seed}" + ("_strict" if STRICT else ""))
    os.makedirs(logdir, exist_ok=True)
    # conf must match this m (main() regenerates per m; guard standalone use)
    conf0 = os.path.join(rundir, "hotstuff.conf")
    cur = None
    if os.path.exists(conf0):
        mt = re.search(r"^block-size\s*=\s*(\d+)", open(conf0).read(), re.M)
        cur = int(mt.group(1)) if mt else None
    if cur != m:
        gen_conf(m, rundir)
    spine, groups = group_layout(m)
    total = spine + sum(groups)

    env = dict(os.environ, FO_DBG_ROWS="1", FO_DBG_FULL="1", FLASHORDER_P_KD=str(KD))
    if STRICT:
        env["FLASHORDER_P_STRICT_REPAIR"] = "1"
    procs = []
    for i in range(N):
        lf = open(os.path.join(logdir, f"rep{i}.log"), "w")
        procs.append((subprocess.Popen(
            [APP, "--conf", os.path.join(rundir, f"hotstuff-sec{i}.conf")],
            env=env, stdout=lf, stderr=lf), lf))
    time.sleep(10)

    cmd = [EVIL, "--idx", "0", "--cid", "0", "--cycles", "1",
           "--spine", str(spine), "--group-sizes", ",".join(map(str, groups)),
           "--max-async", str(total + 50)]
    if arm == "control":
        cmd += ["--no-rotate", "1"]
    clf = open(os.path.join(logdir, "client.log"), "w")
    try:
        subprocess.run(cmd, timeout=60, stdout=clf, stderr=clf, env=env, check=False)
    except subprocess.TimeoutExpired:
        pass
    clf.close()
    time.sleep(3)
    for p, lf in procs:
        p.send_signal(signal.SIGKILL)
        lf.close()
    clean()

    # ---- parse logs: find leader with FINAL_ORDER_FULL
    rows_dbg = fin_full = None
    leader = None
    row_events, prof, prop, nclusters, scc = [], None, None, None, None
    for i in range(N):
        path = os.path.join(logdir, f"rep{i}.log")
        text = open(path, errors="ignore").read()
        if FIN_RE.search(text):
            leader = i
            fp = {}
            for p, t, hs in FIN_RE.findall(text):
                fp[int(p)] = (int(t), hs.split(","))
            fin_full = []
            for p in range(fp[0][0]):
                fin_full += fp[p][1]
            # reassemble chunked rows (parts in file order)
            parts = [(int(f), int(p), int(t), hs.split(","))
                     for f, p, t, hs in ROW_RE.findall(text)]
            byrow = {}
            for f, p, t, hs in parts:
                byrow.setdefault(f, {})[p] = (t, hs)
            sort_rows = SORTROWS_RE.search(text)
            used_ids = ([int(x.split(":")[0]) for x in sort_rows.group(2).split(",")]
                        if sort_rows and sort_rows.group(2) else None)
            row_events = []
            for f, d in byrow.items():
                if set(d) != set(range(d[0][0] if d else 0)) or not d:
                    continue                       # incomplete row: skip
                n_parts = next(iter(d.values()))[0]
                hs = []
                for p in range(n_parts):
                    hs += d[p][1]
                row_events.append((f, len(hs), hs))
            prof = PROF_RE.search(text)
            prop = PROP_RE.search(text)
            cl = CLUSTER_RE.findall(text)
            nclusters = int(cl[0]) if cl else None
            scc = SCC_RE.search(text)
            strep = STRICT_RE.search(text)
            envt = None
            # ENVELOPE_DBG lives in client log
            break
    ctext = open(os.path.join(logdir, "client.log"), errors="ignore").read()

    lat_m = CLI_LAT_RE.search(ctext)
    lat_p50, lat_p95 = (float(lat_m.group(2)), float(lat_m.group(3))) if lat_m else (None, None)

    sp_parts = {int(p): hs.split(",") for p, t, hs in ENV_SPINE_RE.findall(ctext)}
    assert sp_parts, "no ENVELOPE_DBG in client log"
    spine_hex = [h for p in sorted(sp_parts) for h in sp_parts[p]]
    gparts = {}
    for g, sz, p, t, hs in ENV_GROUP_RE.findall(ctext):
        gparts.setdefault(int(g), {"size": int(sz), "parts": {}})
        gparts[int(g)]["parts"][int(p)] = hs.split(",")
    groups_hex = [(gparts[g]["size"],
                   [h for p in sorted(gparts[g]["parts"]) for h in gparts[g]["parts"][p]])
                  for g in sorted(gparts)]
    tx_hex = spine_hex + [h for _, hs in groups_hex for h in hs]
    idx_of = {h: i for i, h in enumerate(tx_hex)}
    # foreign txs (e.g. app-initiated warmup cmd) genuinely present in the
    # batch get sequential identities appended — rows/final stay ground truth
    next_id = [len(tx_hex)]
    def ident(h):
        if h not in idx_of:
            idx_of[h] = next_id[0]
            tx_hex.append(h)
            next_id[0] += 1
        return idx_of[h]
    m_actual = len(tx_hex)

    # ---- rows actually consumed by the C++ sort (subset at propose time),
    # in the exact collection order logged by [SORT_ROWS]
    if used_ids:
        order_ids = [r for r in used_ids if any(ev[0] == r for ev in row_events)]
        byid = {ev[0]: ev for ev in row_events}
        row_ref = [byid[r] for r in order_ids]
    else:
        row_ref = row_events
    # C++ assigns tx ids by first appearance scanning these rows in order,
    # and ties in the internal sort break on that id -> replay must relabel
    cpp_id, inv = {}, {}
    for ev in row_ref:
        for h in ev[2]:
            if h not in cpp_id:
                cpp_id[h] = len(cpp_id)
                inv[cpp_id[h]] = h
    rows_idx = [[cpp_id[h] for h in ev[2]] for ev in row_ref]
    fin_idx = [cpp_id[h] for h in fin_full]
    m_actual = len(cpp_id)

    # ---- offline PD replay on the exact captured rows
    f_cpp = 7                                        # structural: 21 - nmajority(14)
    sorter = PDSorter(N, f_cpp, gamma=GAMMA, k_d=KD, margin=1.2)
    rep = sorter.sort(rows_idx)
    py_order_hex = [inv[t] for t in rep["order"]]
    hash_cpp = sha_of(fin_full)
    hash_py = sha_of(py_order_hex)
    hash_match = int(hash_cpp == hash_py)

    # ---- metrics (threshold identical to C++)
    thr = rep["threshold"]
    pref = rep["pref"]
    pos = np.empty(m_actual, dtype=np.int64)
    pos[fin_idx] = np.arange(m_actual)
    maj = np.maximum(pref, pref.T)
    adm = maj >= thr
    np.fill_diagonal(adm, False)
    fwd = pref >= pref.T
    a, b = np.triu_indices(m_actual, k=1)
    ok = np.where(fwd[a, b], pos[a] < pos[b], pos[b] < pos[a])
    cl = np.empty(m_actual, dtype=np.int64)
    for ci, g in enumerate(rep["clusters"]):
        cl[g] = ci
    same = cl[a] == cl[b]
    sel = adm[a, b]
    n_qual = int(sel.sum())
    n_viol = int((~ok & ~same)[sel].sum())          # absorbed semantics = satisfied
    n_viol_strict = int((~ok)[sel].sum())
    L = n_viol / n_qual if n_qual else 0.0
    L_strict = n_viol_strict / n_qual if n_qual else 0.0

    # MaxDisp vs first observation row
    ref = np.empty(m_actual, dtype=np.int64)
    ref[np.array(rows_idx[0])] = np.arange(m_actual)
    dmax = int(np.abs(pos - ref).max())

    sizes = [len(g) for g in rep["clusters"]]
    n_final_clusters = len(fin_full) and len(sizes)
    result = {
        "m": m, "arm": arm, "seed": seed,
        "n_rows": len(rows_idx), "n_rows_cpp": int(sort_rows.group(1)) if sort_rows else None,
        "q_gamma": thr,
        "qualified_pairs": n_qual,
        "violated_pairs_absorbed": n_viol, "violated_pairs_strict": n_viol_strict,
        "L_gamma": round(L, 4), "L_gamma_strict": round(L_strict, 4),
        "RPS_gamma": round(1 - L, 4),
        "C_max": max(sizes), "C_max_over_m": round(max(sizes) / m_actual, 4),
        "D_max": dmax, "D_max_norm": round(dmax / (m_actual - 1), 4),
        "final_clusters": len(sizes),
        "quotient_compression": round(len(sizes) / m_actual, 4),
        "merge_rounds": int(strep.group(1)) if strep else int(rep["qscc_merged"]),
        "repair_merges": int(strep.group(2)) if strep else 0,
        "strict_mode": int(STRICT),
        "cpp_py_hash_match": hash_match,
        "hash_cpp": hash_cpp[:16], "hash_py": hash_py[:16],
        "t_prefs_ms": float(prof.group(1)) if prof else None,
        "t_canon_ms": float(prof.group(2)) if prof else None,
        "t_clusters_ms": float(prof.group(3)) if prof else None,
        "t_hypergraph_ms": float(prof.group(4)) if prof else None,
        "t_topo_ms": float(prof.group(5)) if prof else None,
        "t_propose_ms": float(prop.group(2)) if prop else None,
        "client_lat_p50_ms": lat_p50,
        "client_lat_p95_ms": lat_p95,
        "leader": leader,
    }
    return result


def main():
    rundir = HERE
    results = []
    for m in MS:
        clean()
        gen_conf(m, rundir)
        for arm in ("control", "cycle"):
            for seed in SEEDS:
                r = run_one(m, seed, arm, rundir)
                results.append(r)
                print(f"[m={m} {arm} s{seed}] L={r['L_gamma']} "
                      f"Cmax/m={r['C_max_over_m']} Dnorm={r['D_max_norm']} "
                      f"clusters={r['final_clusters']} hash_match={r['cpp_py_hash_match']}")
    out = os.path.join(HERE, "envelope_results.csv")
    with open(out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(results[0].keys()))
        w.writeheader()
        w.writerows(results)
    meta = {
        "experiment": "29_envelope_21node_macro_cycle",
        "config": {"n": N, "gamma": GAMMA, "f_struct": 7, "q_gamma": 16,
                   "groups": L_GROUPS, "ms": MS, "seeds": SEEDS, "kd": KD,
                   "strict_repair": False, "path": "local loopback 21 replicas"},
        "pd_core": os.path.relpath(PD_CORE_DIR, HERE),
        "hash_rule": "sha256 over comma-joined full tx hashes in final order",
        "results_csv": os.path.basename(out),
    }
    with open(os.path.join(HERE, "envelope_metadata.json"), "w") as fh:
        json.dump(meta, fh, indent=2, ensure_ascii=False)
    print("wrote", out)


if __name__ == "__main__":
    main()
