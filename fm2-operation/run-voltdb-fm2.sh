#!/bin/bash
set -euo pipefail
set -x

WKLD_LIST=(tpcc) # b c d e f)
PING_IP=10.0.0.1   # IP to validate VM network
PING_RETRIES=5            # max ping attempts before giving up
MIGRATION_TIMEOUT=60      # seconds to wait for new qemu-system PID

trap 'pkill -f "^promo " 2>/dev/null || true
      screen -wipe >/dev/null 2>&1 || true' EXIT

# ─── Helper functions ──────────────────────────────────────────────────────────
launch_src_vm() {
    local session=$1
    screen -dmS "$session" \
        ./apps/controller shm src apps/vm-boot/voltdb.exp 4 20G vm_src.txt 1000000
        sleep 30
}

wait_for_ping() {
    local tries=0
    until ping -c1 -W2 "$PING_IP" >/dev/null 2>&1; do
        ((tries++))
        [[ $tries -ge $PING_RETRIES ]] && return 1
        sleep 20
    done
}

wait_for_migration() {
    local old_pid=$1
    local elapsed=0
    while (( elapsed < MIGRATION_TIMEOUT )); do
        if pgrep qemu-system | grep -v "$old_pid" >/dev/null; then
            return 0
        fi
        sleep 1
        ((elapsed++))
    done
    return 1
}

# ─── Main loop ─────────────────────────────────────────────────────────────────
for wkld in "${WKLD_LIST[@]}"; do
    SRC_SESSION="vm_src_${wkld}"
    DST_SESSION="vm_dst_${wkld}"
    BAK_SESSION="vm_backup_${wkld}"

    # Clean any stale screen sessions
    screen -S "$SRC_SESSION" -X quit 2>/dev/null || true
    screen -S "$DST_SESSION" -X quit 2>/dev/null || true
    screen -S "$BAK_SESSION" -X quit 2>/dev/null || true

    # -------- Launch source VM with network verification ----------------------
    until launch_src_vm "$SRC_SESSION" && wait_for_ping; do
        echo "Ping failed; restarting source VM."
        ./scripts/my_kill.sh
        screen -S "$SRC_SESSION" -X quit 2>/dev/null || true
    done
    src_pid=$(pgrep qemu-system | head -n1)

    screen -dmS "$DST_SESSION" ./apps/controller shm dst 4 20G vm_dst.txt 1342177280B

	sleep 10

    # -------- Optional backup VM ---------------------------------------------
    screen -dmS "$BAK_SESSION" bash -c "./apps/controller shm backup 20 > fm2_voltdb_downtime_wkld_tpcc.dat 2>&1"

    # -------- Workload --------------------------------------------------------

    ./tpcc_load.sh
    sleep 10
    ./tpcc_run.sh 300 100 1000 | tee fm2_voltdb_perf_wkld_tpcc.dat &
    wkld_pid=$!
    sleep 100

    [[ -f controller.pid ]] || { echo "controller.pid missing"; ./scripts/my_kill.sh; exit 1; }
    sudo kill -SIGUSR1 "$(cat controller.pid)"

    sleep 120  # workload run time

    # -------- Teardown --------------------------------------------------------
    ./scripts/my_kill.sh
    sleep 10
done

