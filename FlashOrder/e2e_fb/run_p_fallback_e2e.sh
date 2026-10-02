#!/bin/bash
# run_p_fallback_e2e.sh -- flashorder-P 本地 9 节点 fallback e2e
# 用法: KD=1.0 SKEW=0.99 PROB=0.95 ASYNC=1600 DUR=20 bash run_p_fallback_e2e.sh <tag>
set -u
cd "$(dirname "$0")"
TAG=${1:-run}
KD=${KD:-1.0}
SKEW=${SKEW:-0.99}
PROB=${PROB:-0.95}
ASYNC=${ASYNC:-1600}
DUR=${DUR:-20}
N=${N:-9}
BS=${BS:-200}
STRICT=${STRICT:-0}
SENV=""
[ "$STRICT" = "1" ] && SENV="FLASHORDER_P_STRICT_REPAIR=1"

pkill -9 -f '[f]lashorder-P-app' 2>/dev/null; pkill -9 -f '[f]lashorder-P-client' 2>/dev/null
sleep 1
mkdir -p runs/$TAG; rm -f runs/$TAG/*.log

# 按需重生成配置（skew/prob/N/BS 变化时）
CACHED_N=$(wc -l < _ips.txt 2>/dev/null || echo 0)
CACHED_BS=$(grep -oP '^block-size\s*=\s*\K\d+' hotstuff.conf 2>/dev/null || echo 0)
if [ ! -f hotstuff.conf ] || [ "${REGEN:-0}" = "1" ] || [ "$CACHED_N" != "$N" ] || [ "$CACHED_BS" != "$BS" ]; then
  printf '127.0.0.1\n%.0s' $(seq $N) > _ips.txt
  python3 ../scripts/gen_conf.py --prefix hotstuff --ips _ips.txt \
    --keygen ../build/flashorder-P-keygen --tls-keygen ../build/flashorder-P-tls-keygen \
    --block-size $BS --nworker 4 --repnworker 4 --clinworker 4 \
    --sb-users 100000 --sb-prob-choose_mtx $PROB --sb-skew-factor $SKEW \
    --fairness-parameter ${GAMMA:-0.8} --pace-maker rr >/dev/null
  sed -i '/^tls-privkey/d; /^tls-cert/d' hotstuff-sec*.conf 2>/dev/null
fi

for i in $(seq 0 $((N-1))); do
  env $SENV FLASHORDER_P_KD=$KD ../build/examples/flashorder-P-app --conf hotstuff-sec${i}.conf > runs/$TAG/rep${i}.log 2>&1 &
done
sleep 8
timeout -k 3 $DUR ../build/examples/flashorder-P-client --idx 0 --iter -1 --max-async $ASYNC > runs/$TAG/client.log 2>&1
pkill -9 -f '[f]lashorder-P-app' 2>/dev/null
sleep 1

python3 ../../remote-test/p_fallback_stats.py runs/$TAG runs/$TAG/client.log
