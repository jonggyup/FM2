#!/bin/bash
set -o pipefail
set -x

WKLD_LIST=(a b c d e f)
PING_IP=10.0.0.1      # IP to validate VM network
PING_RETRIES=5            # max ping attempts before giving up
MIGRATION_TIMEOUT=700     # seconds to wait for migration completion by file
YCSB_START_TIMEOUT=${YCSB_START_TIMEOUT:-120}
YCSB_WARMUP_SECONDS=${YCSB_WARMUP_SECONDS:-10}
REDIS_TIMEOUT_MS=${REDIS_TIMEOUT_MS:-15000}
# Pace background FMSync copying so the source vCPUs and Redis retain CPU and
# memory bandwidth.  Set to 0 to disable the cap.
FM_FMSYNC_BANDWIDTH_BPS=${FM_FMSYNC_BANDWIDTH_BPS:-0}

trap 'pkill -f "^promo " 2>/dev/null || true
      screen -wipe >/dev/null 2>&1 || true' EXIT

# ─── Helper functions ──────────────────────────────────────────────────────────
launch_src_vm() {
    local session=$1
    local output_file=$2
    screen -dmS "$session" \
        ./apps/controller shm src apps/vm-boot/redis.exp 4 20G "$output_file" 5000000
    sleep 60
}

wait_for_ping() {
    local tries=0
    until ping -c1 -W2 "$PING_IP" >/dev/null 2>&1; do
        ((tries++))
        [[ $tries -ge $PING_RETRIES ]] && return 1
        sleep 20
    done
}

# Start migration relative to the actual benchmark start, not Maven/YCSB
# startup time.  Also notice if the benchmark exits before it becomes ready.
wait_for_ycsb_start() {
    local log_file=$1
    local benchmark_pid=$2
    local elapsed=0

    while (( elapsed < YCSB_START_TIMEOUT )); do
        if [[ -f "$log_file" ]] && grep -q "Starting test\." "$log_file"; then
            return 0
        fi
        if ! kill -0 "$benchmark_pid" 2>/dev/null; then
            echo "YCSB exited before reporting that the test started." >&2
            return 1
        fi
        sleep 1
        ((elapsed++))
    done

    echo "Timed out waiting for YCSB to start after ${YCSB_START_TIMEOUT}s." >&2
    return 1
}

# Wait for migration by tailing the per-workload downtime log for the marker line.
wait_for_migration_by_file() {
    local file="$1"
    local timeout="${2:-$MIGRATION_TIMEOUT}"
    local elapsed=0
    while (( elapsed < timeout )); do
        if [[ -f "$file" ]] && grep -q "Migration end" "$file"; then
            return 0
        fi
        sleep 1
        ((elapsed++))
    done
    return 1
}

export CXLSHM_PATH=/dev/shm/my_shared_memory   # optional; this is default
export CXLSHM_NODE=1                          # ensure allocation on node 2
export FM_FMSYNC_BANDWIDTH_BPS
export REDIS_TIMEOUT_MS

failed_workloads=()

# ─── Main loop ─────────────────────────────────────────────────────────────────
for wkld in "${WKLD_LIST[@]}"; do
    SRC_SESSION="vm_src_${wkld}"
    DST_SESSION="vm_dst_${wkld}"
    BAK_SESSION="vm_backup_${wkld}"
    DONEFILE="fm2_redis_downtime_${wkld}.dat"
    PERF_FILE="fm2_redis_perf_wkld_${wkld}.dat"
    SRC_LOG="vm_src_redis_${wkld}.txt"
    DST_LOG="vm_dst_redis_${wkld}.txt"

    rm -f /dev/shm/my_shared_memory

    # Clean any stale screen sessions
    screen -S "$SRC_SESSION" -X quit 2>/dev/null || true
    screen -S "$DST_SESSION" -X quit 2>/dev/null || true
    screen -S "$BAK_SESSION" -X quit 2>/dev/null || true

    # Clean stale migration-done file to avoid false positives
    rm -f "$DONEFILE" || true

    # -------- Launch source VM with network verification ----------------------
    until launch_src_vm "$SRC_SESSION" "$SRC_LOG" && wait_for_ping; do
        echo "Ping failed; restarting source VM."
        ./scripts/my_kill.sh
        screen -S "$SRC_SESSION" -X quit 2>/dev/null || true
    done

    sleep 10

    # -------- Workload --------------------------------------------------------
    ./redis_load.sh "$wkld"
    sleep 10

    # -------- Launch destination VM and start backup logger -------------------
    screen -dmS "$DST_SESSION" ./apps/controller shm dst 4 20G "$DST_LOG" 1342177280B
    sleep 10

    screen -dmS "$BAK_SESSION" bash -c \
    "exec ./apps/controller shm backup 30 > fm2_redis_downtime_${wkld}.dat 2>&1"

    sleep 10
    { ./redis_run.sh "$wkld" | tee "$PERF_FILE"; } &
    redis_run_pid=$!

    if ! wait_for_ycsb_start "$PERF_FILE" "$redis_run_pid"; then
        failed_workloads+=("$wkld:start")
        sudo ./scripts/my_kill.sh
        sudo pkill -ef java || true
        continue
    fi
    sleep "$YCSB_WARMUP_SECONDS"

    [[ -f controller.pid ]] || { echo "controller.pid missing"; ./scripts/my_kill.sh; exit 1; }
    sudo kill -SIGUSR1 "$(cat controller.pid)"

    sleep 120
	
    sudo ./scripts/my_kill.sh
    sudo pkill -ef java || true
    wait "$redis_run_pid" 2>/dev/null || true

    # Keep the historical names as the most recent run, while retaining the
    # workload-specific logs needed to distinguish VM and client failures.
    cp -f "$SRC_LOG" vm_src.txt 2>/dev/null || true
    cp -f "$DST_LOG" vm_dst.txt 2>/dev/null || true

    if grep -Eq "JedisConnectionException|SocketTimeoutException|Redis benchmark failed" "$PERF_FILE"; then
        echo "Workload $wkld reported a Redis client failure; see $PERF_FILE." >&2
        failed_workloads+=("$wkld:client")
    fi
done

if (( ${#failed_workloads[@]} > 0 )); then
    printf 'Redis/FM2 failures: %s\n' "${failed_workloads[*]}" >&2
    exit 1
fi
