#!/bin/bash
# Arm A (control) 性能 benchmark：正常 client + SmallBank，无攻击、默认多数行(15)。
# 3 次重复，每次 4000 tx（5 块 × 800）。
set -u
cd "$(dirname "$0")"
APP=../build/examples/flashorder-P-app
CLIENT=../build/examples/flashorder-P-client
N=21
ITER=4000
MAX_ASYNC=800
GAMMA=0.8

RESULTS=arm_a_results.csv
echo "run,n_tx,tps,span_s,p50_ms,p95_ms,n_blocks,D_t_mean,D_t_max,clusters_mean,multi_blocks,max_scc,propose_ms_mean" > "$RESULTS"

for RUN in 1 2 3; do
    echo "===== Arm A run $RUN ====="
    pkill -9 -f flashorder-P-app 2>/dev/null
    pkill -9 -f flashorder-P-client 2>/dev/null
    sleep 1
    rm -f replica_*.log client.log

    for i in $(seq 0 $((N-1))); do
        FLASHORDER_P_KD=0.0 "$APP" --conf "hotstuff-sec${i}.conf" > "replica_${i}.log" 2>&1 &
    done
    sleep 28

    timeout 120 "$CLIENT" --idx 0 --iter "$ITER" --max-async "$MAX_ASYNC" \
        --sb-users 100000 --sb-prob-choose_mtx 0.9 --sb-skew-factor 0.5 \
        --fairness-parameter "$GAMMA" > client.log 2>&1
    echo "   client 退出码: $?"

    python3 analyze_arm_a.py "$RUN" >> "$RESULTS"

    pkill -9 -f flashorder-P-app 2>/dev/null
    pkill -9 -f flashorder-P-client 2>/dev/null
done

echo "===== 完成 ====="
column -s, -t < "$RESULTS"
