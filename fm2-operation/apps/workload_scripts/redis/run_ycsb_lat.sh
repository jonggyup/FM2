#!/bin/bash

# --- Graceful Shutdown Trap ---
# This is the key to making Ctrl-C work correctly. It catches the signal,
# tells the child processes to terminate, and then waits for them to finish.
cleanup() {
    echo -e "\nCaught signal. Sending SIGINT to YCSB process..."
    # Send SIGINT to all direct children of this script (ycsb and awk)
    pkill -INT -P $$
    echo "Waiting for process to flush data and terminate..."
    # Wait for the background jobs to finish their shutdown
    wait
    echo "YCSB process terminated. Exiting."
    exit 0
}

# Set the trap to call the 'cleanup' function on these signals
trap cleanup SIGINT SIGTERM
# --- End of Trap ---


source config.txt

if [ $# -lt 5 ]; then
    echo "Error: No parameters provided."
    echo "Usage: $0 <workloada/b/c/d/e/f> <vCPUs=1> <# of threads> <recordcount> <operationcount>"
    exit 1
fi

workload=$1
# vCPUs argument ($2) is noted but not used in the logic
threads=$3
recordcount=$4
operationcount=$5

cd apps/ycsb

port=12345
out_csv="/home/jonggyu/working/qemu-study/${6}_redis_lat_wkld_${workload}.dat"
log_err="/home/jonggyu/working/qemu-study/ycsb_${workload}.stderr"

echo "Starting YCSB client..."

# Run the single client in the background
stdbuf -oL ./bin/ycsb run redis \
    -P "workloads/$workload" \
    -threads "$threads" \
    -p redis.host="$VM_IP" \
    -p redis.port="$port" \
    -p recordcount="$recordcount" \
    -p operationcount="$operationcount" \
    -p measurementtype=raw 2> "$log_err" > "$out_csv" &

echo "Benchmark client running with PID $$. Press Ctrl-C to stop gracefully."

# 'wait' will pause the script here until the background job finishes
# or until a signal is caught by the trap.
wait

echo "Benchmark finished naturally."

