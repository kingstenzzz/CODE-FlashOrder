#!/bin/bash
set -euo pipefail

# Minimal local four-replica smoke/e2e test for DoD-HS.
# The ordering layer uses block-size transactions per round, so the default
# workload deliberately sends one full block plus a small pending tail.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build}"
RUN_DIR="${RUN_DIR:-$SCRIPT_DIR/test-output/4node}"
ITER="${ITER:-120}"
MAX_ASYNC="${MAX_ASYNC:-120}"
CLIENT_SECONDS="${CLIENT_SECONDS:-25}"
BLOCK_SIZE="${BLOCK_SIZE:-100}"

mkdir -p "$RUN_DIR"
rm -f "$RUN_DIR"/replica{0,1,2,3}.log "$RUN_DIR"/client.log "$RUN_DIR"/gen_conf.log

printf '127.0.0.1\n%.0s' {1..4} > "$RUN_DIR/ips.txt"
python3 scripts/gen_conf.py \
    --prefix hotstuff \
    --ips "$RUN_DIR/ips.txt" \
    --pport "${PPORT:-10000}" \
    --cport "${CPORT:-20000}" \
    --keygen "$BUILD_DIR/dod-hs-keygen" \
    --tls-keygen "$BUILD_DIR/dod-hs-tls-keygen" \
    --nodes nodes.txt \
    --block-size "$BLOCK_SIZE" \
    --pace-maker rr \
    --nworker 8 \
    --repnworker 8 \
    --clinworker 4 \
    --repburst 1000 \
    --cliburst 1000 \
    > "$RUN_DIR/gen_conf.log" 2>&1

replica_pids=()
cleanup() {
    if [[ -n "${client_pid:-}" ]]; then
        kill -TERM "$client_pid" 2>/dev/null || true
    fi
    for pid in "${replica_pids[@]:-}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in "${replica_pids[@]:-}"; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    wait "${client_pid:-}" 2>/dev/null || true
    for pid in "${replica_pids[@]:-}"; do
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

for i in 0 1 2 3; do
    "$BUILD_DIR/examples/dod-hs-app" \
        --conf "hotstuff-sec${i}.conf" \
        --notls \
        > "$RUN_DIR/replica${i}.log" 2>&1 &
    replica_pids+=("$!")
done

sleep 4
"$BUILD_DIR/examples/dod-hs-client" \
    --iter "$ITER" \
    --max-async "$MAX_ASYNC" \
    > "$RUN_DIR/client.log" 2>&1 &
client_pid="$!"

sleep "$CLIENT_SECONDS"
kill -TERM "$client_pid" 2>/dev/null || true
wait "$client_pid" 2>/dev/null || true
unset client_pid

confirmed=$(grep -c '\[hotstuff info\]' "$RUN_DIR/client.log" || true)
_base_p="${PPORT:-10000}"
listening=$( (grep -h -c -E "listening to ($_base_p|$((_base_p+1))|$((_base_p+2))|$((_base_p+3)))" "$RUN_DIR"/replica*.log || true) | awk -F: '{s += $NF} END {print s + 0}')
full_blocks=$( (grep -h -c "stage=vote_decision.*txns=$BLOCK_SIZE" "$RUN_DIR"/replica*.log || true) | awk -F: '{s += $NF} END {print s + 0}')
fairness_stages=$( (grep -h -c 'stage=fairness' "$RUN_DIR"/replica*.log || true) | awk -F: '{s += $NF} END {print s + 0}')

printf 'run_dir=%s\n' "$RUN_DIR"
printf 'replicas_listening=%s/4\n' "$listening"
printf 'full_blocks=%s\n' "$full_blocks"
printf 'fairness_stages=%s\n' "$fairness_stages"
printf 'client_confirmations=%s\n' "$confirmed"

if [[ "$listening" -lt 4 || "$full_blocks" -lt 1 || "$fairness_stages" -lt 1 || "$confirmed" -lt "$BLOCK_SIZE" ]]; then
    echo 'DoD-HS 4-node e2e test: FAIL'
    exit 1
fi

echo 'DoD-HS 4-node e2e test: PASS'
