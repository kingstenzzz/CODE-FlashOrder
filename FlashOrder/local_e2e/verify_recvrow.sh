#!/bin/bash
# 单次验证 RECV_ROW 采集是否生效（control, seed=0）
set -u
cd "$(dirname "$0")"
APP=../build/examples/flashorder-P-app
CLIENT=../build/examples/evil-client

pkill -9 -f flashorder-P-app 2>/dev/null
pkill -9 -f evil-client 2>/dev/null
sleep 1
rm -f replica_*.log client.log

python3 make_rows.py --arm control --seed 0 --out rows.txt >/dev/null
for i in $(seq 0 20); do
    FLASHORDER_P_KD=0.0 FO_WAIT_ALL_ROWS=1 FO_DBG_FULL=1 FO_DBG_RECV_ROWS=1 \
        "$APP" --conf "hotstuff-sec${i}.conf" > "replica_${i}.log" 2>&1 &
done
sleep 28
timeout 60 "$CLIENT" --idx 0 --rows-file rows.txt \
    --sb-users 100000 --sb-prob-choose_mtx 0.9 --sb-skew-factor 0.5 \
    --fairness-parameter 0.8 --max-cli-msg 65536 > client.log 2>&1
sleep 10

echo "=== RECV_ROW 行数（各 replica）==="
for i in $(seq 0 20); do
    c=$(grep -ac "RECV_ROW" "replica_${i}.log" 2>/dev/null)
    [ "$c" -gt 0 ] && echo "  replica_${i}: $c 行"
done
echo "=== leader 的 RECV_ROW 前 2 行 ==="
grep -ah "RECV_ROW" replica_*.log 2>/dev/null | head -2

pkill -9 -f flashorder-P-app 2>/dev/null
pkill -9 -f evil-client 2>/dev/null
echo "done"
