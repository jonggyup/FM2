#!/bin/bash

if [ -z "$1" ]; then
  echo "No QEMU keyword was provided: \`src\` or \`dst\`"
  exit 1
else
  echo "QEMU keyword: $1"
fi

source config.txt

KEY_WORD=$1
QEMU_PID=`ps -eaf | grep qemu-monitor-migration-$KEY_WORD | grep -v grep | awk '{print $2}' | sort -n | tail -1`

THREADS=`ps -L -o pid,tid,comm -p $QEMU_PID | grep -E 'qemu-system-x86' | awk '{print $2}'`

echo QEMU pid: $QEMU_PID
echo QEMU Threads: $THREADS

if [ "$1" == "src" ]; then
    numbers=$(echo "$SRC_CPUSET" | tr ',' ' ')
elif [ "$1" == "dst" ]; then
    numbers=$(echo "$DST_CPUSET" | tr ',' ' ')
else
    echo "Invalid parameter. Use 'src' or 'dst'."
    exit 1
fi

cpu_cores=($numbers)

iter=0
for TID in $THREADS; do
    if [ $iter -ge ${#cpu_cores[@]} ]; then
        echo qemu thread number is larger than core. 
    fi
    taskset -cp ${cpu_cores[$iter]} $TID
    iter=$((iter + 1))
done