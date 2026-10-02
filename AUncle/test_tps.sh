#!/bin/bash
set -e

pkill -9 -f auncel-app 2>/dev/null || true
pkill -9 -f auncel-client 2>/dev/null || true
pkill -9 -f hotstuff-app 2>/dev/null || true
pkill -9 -f hotstuff-client 2>/dev/null || true

cd "$(dirname "$0")"
rm -f log0 log1 log2 log3 client.log client_raw.log

echo "Generating config..."
printf '127.0.0.1\n%.0s' {1..4} > _ips_tmp.txt
python3 scripts/gen_conf.py --prefix hotstuff --ips _ips_tmp.txt --keygen ./build/libhotstuff-keygen --tls-keygen ./build/libhotstuff-tls-keygen --nodes nodes.txt --block-size 100 --pace-maker rr --nworker 8 --repnworker 8 --clinworker 4 --repburst 1000 --cliburst 1000

echo "Starting 4 Auncel replicas..."
for i in {0..3}; do
    ./build/examples/auncel-app --conf hotstuff-sec${i}.conf > log${i}.log 2>&1 &
    echo "  Replica $i started (pid $!)"
done

echo "Waiting for leader election (12s)..."
sleep 12

echo "Starting client for 20s..."
timeout 20 ./build/examples/auncel-client --idx 0 --iter -1 --max-async 400 --weight-k 0.5 > client_raw.log 2>&1 || true

echo "Stopping replicas..."
pkill -9 -f auncel-app 2>/dev/null || true
pkill -9 -f auncel-client 2>/dev/null || true

echo "Extracting throughput data..."
grep "hotstuff info" client_raw.log > client.log || true

echo "Calculating TPS..."
if [ -s client.log ]; then
  python3 scripts/thr_hist.py < client.log || {
    echo "Failed to analyze data. client.log tail:"
    tail -n 5 client.log
  }
else
  echo "client.log is empty, no transactions committed."
  echo "=== Tail of log0 ==="
  tail -n 20 log0.log 2>/dev/null || echo "No log0"
fi

rm -f log0.log log1.log log2.log log3.log
