#!/usr/bin/env python3
"""Live 21-node demo: strict-repair trigger on the real protocol stack.

Deploys n replicas, injects an explicit-rows workload (per-replica permutation)
that contains a both-admitted pair in distinct clusters, and checks whether the
strict repair gate fires (strict_repair: rounds>0 merges>0).

Usage:
  ROWS=trigger_w21_m7_client.txt N=21 BS=7 STRICT=1 bash run_trigger_e2e.sh [tag]
"""
import csv, glob, os, re, signal, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
E2E = os.path.dirname(HERE)
APP = os.path.join(E2E, "..", "build", "examples", "flashorder-P-app")
EVIL = os.path.join(E2E, "..", "build", "examples", "evil-client")

N = int(os.environ.get("N", "21"))
BS = int(os.environ.get("BS", "7"))
STRICT = os.environ.get("STRICT", "1") == "1"
WAIT_ALL = os.environ.get("WAIT_ALL", "1") == "1"
ROWS = os.environ.get("ROWS", "trigger_w21_m7_client.txt")
GAMMA = float(os.environ.get("GAMMA", "1.0"))
TAG = sys.argv[1] if len(sys.argv) > 1 else "trigger"
ROWS_PATH = os.path.join(HERE, ROWS)
LOG = os.path.join(HERE, f"live_{TAG}")


def clean():
    subprocess.run(["pkill", "-9", "-x", "flashorder-P-ap"], capture_output=True)
    subprocess.run(["pkill", "-9", "-x", "evil-client"], capture_output=True)
    time.sleep(1.5)


def gen_conf():
    ips = os.path.join(HERE, "_ips.txt")
    with open(ips, "w") as fh:
        fh.write("127.0.0.1\n" * N)
    subprocess.run([sys.executable, os.path.join(E2E, "..", "scripts", "gen_conf.py"),
                    "--prefix", "hotstuff", "--ips", ips,
                    "--keygen", os.path.join(E2E, "..", "build", "flashorder-P-keygen"),
                    "--tls-keygen", os.path.join(E2E, "..", "build", "flashorder-P-tls-keygen"),
                    "--block-size", str(BS), "--nworker", "2", "--repnworker", "2",
                    "--clinworker", "2", "--sb-users", "100000",
                    "--sb-prob-choose_mtx", "0.95", "--sb-skew-factor", "0.99",
                    "--fairness-parameter", str(GAMMA), "--pace-maker", "rr"],
                   cwd=HERE, check=True, capture_output=True)
    for f in glob.glob(os.path.join(HERE, "hotstuff-sec*.conf")):
        subprocess.run(["sed", "-i", "/^tls-privkey/d; /^tls-cert/d", f], check=True)


def main():
    os.makedirs(LOG, exist_ok=True)
    for f in glob.glob(os.path.join(LOG, "*.log")):
        os.remove(f)
    clean()
    gen_conf()
    env = dict(os.environ, FO_DBG_ROWS="1", FO_DBG_FULL="1", FLASHORDER_P_KD="1.0")
    if STRICT:
        env["FLASHORDER_P_STRICT_REPAIR"] = "1"
    if WAIT_ALL:
        env["FO_WAIT_ALL_ROWS"] = "1"
    procs = []
    for i in range(N):
        lf = open(os.path.join(LOG, f"rep{i}.log"), "w")
        procs.append((subprocess.Popen(
            [APP, "--conf", os.path.join(HERE, f"hotstuff-sec{i}.conf")],
            env=env, stdout=lf, stderr=lf), lf))
    time.sleep(10)
    clf = open(os.path.join(LOG, "client.log"), "w")
    try:
        subprocess.run([EVIL, "--idx", "0", "--cid", "0", "--cycles", "1",
                        "--rows-file", ROWS_PATH, "--max-async", str(BS * N + 50)],
                       timeout=60, stdout=clf, stderr=clf, env=env, check=False)
    except subprocess.TimeoutExpired:
        pass
    clf.close()
    time.sleep(3)
    for p, lf in procs:
        p.send_signal(signal.SIGKILL)
        lf.close()
    clean()

    # find leader and report
    found = False
    for i in range(N):
        txt = open(os.path.join(LOG, f"rep{i}.log"), errors="ignore").read()
        if "strict_repair:" in txt:
            found = True
            print(f"=== leader rep{i} ===")
            for line in txt.splitlines():
                if any(k in line for k in ("strict_repair:", "SCC stats:", "n_replicas =", "Final order size")):
                    print("  ", line.strip())
            m = re.search(r"\[SORT_ROWS\] n=(\d+)", txt)
            print("  rows used at sort:", m.group(1) if m else "?")
            break
    if not found:
        print("no strict_repair line found — check logs")
    print("logdir:", LOG)


if __name__ == "__main__":
    main()
