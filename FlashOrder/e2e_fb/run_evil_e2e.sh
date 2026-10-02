#!/bin/bash
# run_evil_e2e.sh -- flashorder-P 本地 9 节点 Condorcet 攻击 e2e（evil-client 注入）
# 用法: KD=1.0 CYCLES=2000 CYCLELEN=3 STRICT=1 DUR=20 bash run_evil_e2e.sh <tag>
set -u
cd "$(dirname "$0")"
TAG=${1:-evil}
KD=${KD:-1.0}
CYCLES=${CYCLES:-2000}
CYCLELEN=${CYCLELEN:-3}
ASYNC=${ASYNC:-1600}
DUR=${DUR:-20}
N=${N:-9}
BS=${BS:-200}
STRICT=${STRICT:-0}
SENV=""
[ "$STRICT" = "1" ] && SENV="FLASHORDER_P_STRICT_REPAIR=1"

pkill -x flashorder-P-ap 2>/dev/null; pkill -x evil-client 2>/dev/null
sleep 1
mkdir -p runs/$TAG; rm -f runs/$TAG/*.log

CACHED_N=$(wc -l < _ips.txt 2>/dev/null || echo 0)
CACHED_BS=$(grep -oP '^block-size\s*=\s*\K\d+' hotstuff.conf 2>/dev/null || echo 0)
if [ ! -f hotstuff.conf ] || [ "${REGEN:-0}" = "1" ] || [ "$CACHED_N" != "$N" ] || [ "$CACHED_BS" != "$BS" ]; then
  printf '127.0.0.1\n%.0s' $(seq $N) > _ips.txt
  python3 ../scripts/gen_conf.py --prefix hotstuff --ips _ips.txt \
    --keygen ../build/flashorder-P-keygen --tls-keygen ../build/flashorder-P-tls-keygen \
    --block-size $BS --nworker 4 --repnworker 4 --clinworker 4 \
    --sb-users 100000 --sb-prob-choose_mtx 0.95 --sb-skew-factor 0.99 \
    --fairness-parameter 0.8 --pace-maker rr >/dev/null
  sed -i '/^tls-privkey/d; /^tls-cert/d' hotstuff-sec*.conf 2>/dev/null
fi

for i in $(seq 0 $((N-1))); do
  env $SENV FO_DBG_POS=${FO_DBG_POS:-0} FLASHORDER_P_KD=$KD ../build/examples/flashorder-P-app --conf hotstuff-sec${i}.conf > runs/$TAG/rep${i}.log 2>&1 &
done
sleep 8
timeout -k 3 $DUR ../build/examples/evil-client --idx 0 --cid 0 \
  --cycles $CYCLES --cycle-len $CYCLELEN --rot-mode ${ROTMODE:-group} --max-async $ASYNC \
  > runs/$TAG/client.log 2>&1
pkill -x flashorder-P-ap 2>/dev/null
sleep 1

echo "== evil client 注入统计 =="
grep -c "Injected" runs/$TAG/client.log 2>/dev/null | xargs -I{} echo "注入进度日志: {} 条(每10个cycle)"
grep "All cycles confirmed" runs/$TAG/client.log 2>/dev/null || echo "(未全部确认或超时)"
python3 ../../remote-test/p_fallback_stats.py runs/$TAG runs/$TAG/client.log
