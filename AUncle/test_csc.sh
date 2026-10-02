#!/bin/bash
set -e

pkill -9 -f auncel-app 2>/dev/null || true
pkill -9 -f auncel-client 2>/dev/null || true
sleep 1

cd "$(dirname "$0")"
rm -f /tmp/csc_log*.log /tmp/csc_client*.log

echo "Generating config..."
printf '127.0.0.1\n%.0s' {1..4} > _ips_tmp.txt
python3 scripts/gen_conf.py --prefix hotstuff --ips _ips_tmp.txt --keygen ./build/libhotstuff-keygen --tls-keygen ./build/libhotstuff-tls-keygen --nodes nodes.txt --block-size 100 --pace-maker rr --nworker 8 --repnworker 8 --clinworker 4 --repburst 1000 --cliburst 1000

echo "Starting 4 Auncel replicas..."
for i in {0..3}; do
    ./build/examples/auncel-app --conf hotstuff-sec${i}.conf > /tmp/csc_log${i}.log 2>&1 &
done

echo "Waiting for leader election (15s)..."
sleep 15

echo "Starting client for 15s..."
timeout 15 ./build/examples/auncel-client --idx 0 --iter -1 --max-async 400 > /tmp/csc_client_raw.log 2>&1 || true

echo "Stopping replicas..."
pkill -9 -f auncel-app 2>/dev/null || true
pkill -9 -f auncel-client 2>/dev/null || true

echo "=== Done. Logs at /tmp/csc_log*.log ==="
