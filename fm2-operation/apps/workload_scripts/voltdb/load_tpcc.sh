#!/bin/bash

source config.txt

cd apps/voltdb/voltdb/tests/test_apps/tpcc
sudo bash run.sh init NULL NULL NULL /home/jonggyu/working/qemu-study/config.txt

echo "Finished VoltDB TPC-C loading."
