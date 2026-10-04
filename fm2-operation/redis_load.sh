#!/bin/bash

sudo bash apps/workload_scripts/redis/load_ycsb.sh workload${1} 1 5000000 10 #30000000 for 60G

#sudo bash apps/workload_scripts/redis/load_ycsb.sh workload${1} 1 5000000 20
