#!/bin/bash
# 本地 21 节点 flashorder-P e2e 冒烟：macro-cycle 注入（Arm C 的一次运行）
set -u
cd "$(dirname "$0")"
APP=../build/examples/flashorder-P-app
CLIENT=../build/examples/evil-client
N=21
KD=0.0
GAMMA=0.8

# 清理（用 -x 精确匹配进程名，避免误杀本脚本）
pkill -9 -x flashorder-P-app 2>/dev/null
pkill -9 -x evil-client 2>/dev/null
sleep 1
rm -f replica_*.log client.log

echo "[1/5] 启动 $N 个 replica (k_d=$KD, FO_WAIT_ALL_ROWS, FO_DBG_FULL)"
for i in $(seq 0 $((N-1))); do
    FLASHORDER_P_KD=$KD FO_WAIT_ALL_ROWS=1 FO_DBG_FULL=1 \
        "$APP" --conf "hotstuff-sec${i}.conf" > "replica_${i}.log" 2>&1 &
done

echo "[2/5] 等待 leader 选举与稳定 (30s)..."
sleep 30

alive=0
for i in $(seq 0 $((N-1))); do
    pgrep -x flashorder-P-app >/dev/null 2>&1 && alive=$((alive+1))
done
echo "   存活 replica 进程数: $(pgrep -xc flashorder-P-app)"
echo "   已 commit 的块数(按日志): $(grep -l 'deliver' replica_*.log 2>/dev/null | wc -l)"

echo "[3/5] 注入 clean rotation + f=6 byz certificate (--rows-file, noise=0 + cycle=20)"
python3 make_cycle_rows.py
timeout 60 "$CLIENT" --idx 0 --rows-file rows.txt \
    --sb-users 100000 --sb-prob-choose_mtx 0.9 --sb-skew-factor 0.5 \
    --fairness-parameter $GAMMA --max-cli-msg 65536 > client.log 2>&1
echo "   evil-client 退出码: $?"

echo "[4/5] 等待 block 提交与排序 (10s)..."
sleep 10

echo "[5/5] 采集结果"
echo "=== SCC stats（各 replica 日志）==="
grep -h "SCC stats" replica_*.log 2>/dev/null | tail -8
echo "=== HyperGraph-P 聚类/D_t ==="
grep -h "HyperGraph-P" replica_*.log 2>/dev/null | tail -8
echo "=== FINAL_ORDER 大小 / perf ==="
grep -h "Final order size\|flashorder_propose:" replica_*.log 2>/dev/null | tail -8
echo "=== ENVELOPE_DBG（注入器视角，前 3 行）==="
grep -h "ENVELOPE_DBG\|ROWS_DBG" client.log 2>/dev/null | head -3
echo "=== client 确认/延迟 ==="
grep -h "CLIENT_LAT\|All cycles" client.log 2>/dev/null | tail -3

echo "=== 停机 ==="
pkill -9 -x flashorder-P-app 2>/dev/null
pkill -9 -x evil-client 2>/dev/null
echo "done"
