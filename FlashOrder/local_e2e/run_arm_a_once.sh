#!/bin/bash
set -u
cd "$(dirname "$0")"
APP=../build/examples/flashorder-P-app
CLIENT=../build/examples/flashorder-P-client
pkill -9 -f flashorder-P-app 2>/dev/null
pkill -9 -f flashorder-P-client 2>/dev/null
sleep 1
rm -f replica_*.log client.log
for i in $(seq 0 20); do
    FLASHORDER_P_KD=0.0 "$APP" --conf "hotstuff-sec${i}.conf" > "replica_${i}.log" 2>&1 &
done
sleep 28
timeout 60 "$CLIENT" --idx 0 --iter 4000 --max-async 800 \
    --sb-users 100000 --sb-prob-choose_mtx 0.9 --sb-skew-factor 0.5 \
    --fairness-parameter 0.8 > client.log 2>&1
echo "client 退出码: $?"
python3 analyze_arm_a.py 1
pkill -9 -f flashorder-P-app 2>/dev/null
pkill -9 -f flashorder-P-client 2>/dev/null
