source config.txt

if [ $# -lt 3 ]; then
    echo "Error: No parameters provided."
    echo "Usage: $0 <duration(seconds)> <# of warehouses> <displayinterval(milliseconds)>"
    exit 1
fi

# the first
duration=$1
warehouses=$2
displayinterval=$3

cd apps/voltdb/voltdb/tests/test_apps/tpcc
sudo bash run.sh client $duration $warehouses $displayinterval /home/jonggyu/working/qemu-study/config.txt

echo "Finished VoltDB TPC-C benchmark."
