source config.txt

if [ $# -lt 4 ]; then
    echo "Error: No parameters provided."
    echo "Usage: $0 <workloada/b/c/d/e/f> <# of threads> <recordcount> <operationcount>"
    exit 1
fi

workload=$1
threads=$2
recordcount=$3
operationcount=$4

cd apps/ycsb

sudo ./bin/ycsb run memcached -s -threads $threads \
                                -P workloads/$workload \
                                -p "memcached.hosts=$VM_IP:12345" \
                                -p status.interval=1 \
                                -p "recordcount=$recordcount" \
                                -p "operationcount=$operationcount" \
                                2>&1

echo "Finished Memcached benchmark."