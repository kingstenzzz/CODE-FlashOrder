#!/bin/bash
pkill -9 -f flashorder-D-app 2>/dev/null
sleep 1
cd /home/kkk/code/new-code/Order-Fairness/flashorder-D

echo "=== Phi Sweep: {0.075, 0.15, 0.30} ==="

for PHI in 0.075 0.15 0.30; do
    echo ""
    echo "--- Phi=$PHI ---"
    pkill -9 -f flashorder-D-app 2>/dev/null
    sleep 1
    
    for i in 0 1 2 3; do
        nohup ./build/examples/flashorder-D-app --conf hotstuff-sec${i}.conf --phi $PHI > /tmp/fod_phi${PHI}_log${i}.log 2>&1 &
    done
    sleep 8
    
    echo "  Replicas: $(pgrep -c -f flashorder-D-app)"
    
    timeout 15 ./build/examples/flashorder-D-client --idx 0 --iter 100000 --max-async 500 > /tmp/fod_phi${PHI}_client.log 2>&1
    LINES=$(grep -c "hotstuff info" /tmp/fod_phi${PHI}_client.log 2>/dev/null || echo 0)
    echo "  Client lines: $LINES"
    
    if [ "$LINES" -gt 5 ]; then
        grep "hotstuff info" /tmp/fod_phi${PHI}_client.log > /tmp/fod_phi${PHI}_parsed.log
        python3 scripts/thr_hist.py < /tmp/fod_phi${PHI}_parsed.log 2>&1
    fi
    
    echo ""
    echo "  SCC stats from leader log:"
    grep "SCC stats" /tmp/fod_phi${PHI}_log0.log 2>/dev/null | head -5
    
    echo ""
    echo "  Profile from leader log:"
    grep "PROFILE" /tmp/fod_phi${PHI}_log0.log 2>/dev/null | head -5
done

pkill -9 -f flashorder-D-app 2>/dev/null
echo ""
echo "=== Done ==="
