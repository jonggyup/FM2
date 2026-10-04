#!/bin/bash

# Check if an argument is provided
if [ -z "$1" ]; then
    echo "Error: Please provide a workload letter (a, b, c, d, e, or f)."
    echo "Usage: $0 <workload_letter>"
    exit 1
fi

# Set the number of operations based on the first argument
case "$1" in
    a|b|c|d|f)
        OPS_COUNT="20000000"
        ;;
    e)
        OPS_COUNT="2000000"
        ;;
    *)
        echo "Error: Invalid workload '$1'. Please use a, b, c, d, e, or f."
        exit 1
        ;;
esac
REC_COUNT=5000000
REDIS_TIMEOUT_MS=${REDIS_TIMEOUT_MS:-15000}
echo "Running workload${1} with ${OPS_COUNT} operations..."
echo "Redis request timeout: ${REDIS_TIMEOUT_MS} ms"
sudo bash apps/workload_scripts/redis/run_ycsb.sh \
    "workload${1}" 1 2 "${REC_COUNT}" "${OPS_COUNT}" "${REDIS_TIMEOUT_MS}"
