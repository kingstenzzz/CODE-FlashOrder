#!/bin/bash
set -e

cd "$(dirname "$0")"
rm -f /tmp/asan21_log*.log /tmp/asan21_client*.log

export ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:detect_leaks=0

echo "Starting 21 ASAN replicas..."
for i in $(seq 0 20); do
    ./build-asan/examples/auncel-app --conf hotstuff-sec${i}.conf > /tmp/asan21_log${i}.log 2>&1 &
done
echo "PIDs: $(pgrep -f auncel-app | wc -l) running"

echo "Waiting for leader election (25s for 21 nodes)..."
sleep 25

echo "Starting client for 30s..."
timeout 30 ./build-asan/examples/auncel-client --idx 0 --iter -1 --max-async 400 > /tmp/asan21_client_raw.log 2>&1 || true

echo "Stopping replicas..."
pkill -9 -f auncel-app 2>/dev/null || true
pkill -9 -f auncel-client 2>/dev/null || true
sleep 2

echo "=== ASAN error scan ==="
if grep -rl "ERROR: AddressSanitizer\|heap-use-after-free\|attempting free" /tmp/asan21_log*.log /tmp/asan21_client_raw.log 2>/dev/null; then
    echo "!!! ASAN ERRORS FOUND !!!"
    grep -n "ERROR: AddressSanitizer\|SUMMARY\|heap-use-after-free" /tmp/asan21_log*.log /tmp/asan21_client_raw.log | head -20
    exit 1
else
    echo "No ASAN errors detected."
fi

echo "=== TPS calculation ==="
python3 -c "
import re
lines=[l for l in open('/tmp/asan21_client_raw.log') if 'hotstuff info' in l]
if not lines:
    print('no commits'); raise SystemExit
def s(x):
    h,m,sec=x.split(':'); return int(h)*3600+int(m)*60+float(sec)
t=[s(re.search(r'\d{4}-\d{2}-\d{2} (\d{2}:\d{2}:\d{2}\.\d+)',l).group(1)) for l in lines]
print(f'commits={len(t)} span={t[-1]-t[0]:.1f}s TPS={len(t)/(t[-1]-t[0]):.1f}')
"

echo "=== Replica crash check ==="
for i in $(seq 0 20); do
    if ! grep -q "stopping all threads" /tmp/asan21_log${i}.log 2>/dev/null; then
        echo "WARNING: replica $i may have crashed (no clean shutdown log)"
    fi
done
echo "Done."
