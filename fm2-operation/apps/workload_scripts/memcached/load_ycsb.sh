source config.txt

if [ $# -lt 3 ]; then
    echo "Error: No parameters provided."
    echo "Usage: $0 <workloada/b/c/d/e/f> <# of threads> <recordcount>"
    exit 1
fi

workload=$1
threads=$2
recordcount=$3

cd apps/ycsb

sudo ./bin/ycsb load memcached -s -threads $threads \
                                -P workloads/$workload \
                                -p "memcached.hosts=$VM_IP:12345" \
                                -p "recordcount=$recordcount" \

echo "Finished Memcached loading."