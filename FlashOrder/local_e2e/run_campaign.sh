#!/bin/bash
# Local 21-node campaign: control + macro-cycle(20/100) x 3 seeds
set -u
cd "$(dirname "$0")"
APP=../build/examples/flashorder-P-app
CLIENT=../build/examples/evil-client
N=21
KD=0.0
GAMMA=0.8

RESULTS=results.csv
echo "arm,seed,D_t,clusters,n_multi_sccs,max_scc_size,p50_ms,p95_ms,n_rows,m,replay_match" > "$RESULTS"

run_one() {
    local ARM="$1" SEED="$2" CYCLE="$3"
    echo "===== $ARM seed=$SEED ====="
    pkill -9 -f flashorder-P-app 2>/dev/null
    pkill -9 -f evil-client 2>/dev/null
    sleep 1
    rm -f replica_*.log client.log

    if [ "$ARM" = "control" ]; then
        python3 make_rows.py --arm control --seed "$SEED" --out rows.txt
    else
        python3 make_rows.py --arm cycle --cycle "$CYCLE" --seed "$SEED" --out rows.txt
    fi

    for i in $(seq 0 $((N-1))); do
        FLASHORDER_P_KD=$KD FO_WAIT_ALL_ROWS=1 FO_DBG_FULL=1 FO_DBG_RECV_ROWS=1 \
            "$APP" --conf "hotstuff-sec${i}.conf" > "replica_${i}.log" 2>&1 &
    done
    sleep 28

    timeout 60 "$CLIENT" --idx 0 --rows-file rows.txt \
        --sb-users 100000 --sb-prob-choose_mtx 0.9 --sb-skew-factor 0.5 \
        --fairness-parameter "$GAMMA" --max-cli-msg 65536 > client.log 2>&1
    sleep 10

    python3 analyze_recvrow.py "$ARM" "$SEED" >> "$RESULTS"

    pkill -9 -f flashorder-P-app 2>/dev/null
    pkill -9 -f evil-client 2>/dev/null
}

for SEED in 0 1 2; do run_one control "$SEED" 0; done
for SEED in 0 1 2; do run_one cycle20 "$SEED" 20; done
for SEED in 0 1 2; do run_one cycle100 "$SEED" 100; done

echo "===== 完成 ====="
column -s, -t < "$RESULTS"
