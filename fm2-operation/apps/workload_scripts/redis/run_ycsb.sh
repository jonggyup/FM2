#!/bin/bash
set -o pipefail

source config.txt

if [ $# -lt 5 ]; then
    echo "Error: No parameters provided."
    echo "Usage: $0 <workloada/b/c/d/e/f> <# of vCPUS> <# of threads> <recordcount> <operationcount>"
    exit 1
fi

workload=$1 
vCPUs=$2
threads=$3
recordcount=$4
operationcount=$5
redis_timeout_ms=${6:-15000}

if ! [[ "$redis_timeout_ms" =~ ^[1-9][0-9]*$ ]]; then
    echo "Error: redis timeout must be a positive number of milliseconds." >&2
    exit 1
fi

cd apps/ycsb
# sudo ./bin/ycsb load redis -s -P apps/ycsb/workloads/workloada -p "redis.host=$VM_IP" -p "redis.port=12345" &

# Loop from 1 to the number of iterations
pids=()
for ((i=0; i<vCPUs; i++)); do
    # Calculate 12345 + iteration number
    result=$((12345 + i))

    sudo ./bin/ycsb run redis -s -threads $threads \
                                -P workloads/$workload \
                                -p "redis.host=$VM_IP" \
                                -p "redis.port=$result" \
                                -p "recordcount=$recordcount" \
                                -p "operationcount=$operationcount" \
                                -p "redis.timeout=$redis_timeout_ms" \
                                -p status.interval=1 \
                                2>&1 | sed "s/^/[client $i] /" &
    pids+=("$!")
done

status=0
for pid in "${pids[@]}"; do
    wait "$pid" || status=1
done

if (( status != 0 )); then
    echo "Redis benchmark failed." >&2
    exit "$status"
fi

echo "Finished Redis benchmark."
