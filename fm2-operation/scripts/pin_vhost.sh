#!/bin/bash
source config.txt

THREADS=`pgrep -f "^vhost-"`

echo vhost Threads: $THREADS

cpu_cores=($numbers)

for TID in $THREADS; do
    taskset -cp $VHOST_CORE $TID
done