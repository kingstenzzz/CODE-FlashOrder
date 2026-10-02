#!/bin/bash
# ASAN full-load crash reproduction test for Fix #1 (uv handle malloc/free)
set -e

pkill -9 -f auncel-app 2>/dev/null || true
pkill -9 -f auncel-client 2>/dev/null || true
sleep 1

cd "$(dirname "$0")"
rm -f /tmp/asan_log*.log /tmp/asan_client*.log

echo "Generating config..."
printf '127.0.0.1\n%.0s' {1..4} > _ips_tmp.txt
python3 scripts/gen_conf.py --prefix hotstuff --ips _ips_tmp.txt --keygen ./build-asan/libhotstuff-keygen --tls-keygen ./build-asan/libhotstuff-tls-keygen --nodes nodes.txt --block-size 100 --pace-maker rr --nworker 8 --repnworker 8 --clinworker 4 --repburst 1000 --cliburst 1000

echo "Starting 4 ASAN Auncel replicas..."
export ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:detect_leaks=0
for i in {0..3}; do
    ./build-asan/examples/auncel-app --conf hotstuff-sec${i}.conf > /tmp/asan_log${i}.log 2>&1 &
    echo "  Replica $i started (pid $!)"
done

echo "Waiting for leader election (15s)..."
sleep 15

echo "Starting ASAN client for 30s..."
timeout 30 ./build-asan/examples/auncel-client --idx 0 --iter -1 --max-async 400 > /tmp/asan_client_raw.log 2>&1 || true

echo "Stopping replicas..."
pkill -9 -f auncel-app 2>/dev/null || true
pkill -9 -f auncel-client 2>/dev/null || true

echo "=== ASAN error scan ==="
if grep -l "ERROR: AddressSanitizer\|runtime error:\|heap-use-after-free\|attempting free" /tmp/asan_log*.log /tmp/asan_client_raw.log 2>/dev/null; then
    echo "!!! ASAN ERRORS FOUND !!!"
    grep -n "ERROR: AddressSanitizer\|heap-use-after-free\|attempting free\|SUMMARY" /tmp/asan_log*.log /tmp/asan_client_raw.log | head -40
    exit 1
else
    echo "No ASAN errors detected."
fi

echo "=== Commit summary ==="
grep -c "hotstuff info" /tmp/asan_client_raw.log || echo "0 commits"
grep "hotstuff info" /tmp/asan_client_raw.log | tail -3 || true
