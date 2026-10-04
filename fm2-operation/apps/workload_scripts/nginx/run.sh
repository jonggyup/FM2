#!/bin/bash

source /home/jonggyu/working/qemu-study/config.txt

if [ $# -lt 3 ]; then
    echo "Error: No parameters provided."
    echo "Usage: $0 <# of threads> <# of connections> <duration>"
    exit 1
fi


wrk -t$1 -c$2 -d$3 -s apps/nginx/test.lua http://$VM_IP
