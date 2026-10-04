#!/bin/bash
#
# Run CXL-memory experiments for GAPBS workloads
#
# Huaicheng Li <lhcwhu@gmail.com>
#

# Change the following global variables based on your environment
#-------------------------------------------------------------------------------
RUNDIR="~/gapbs"

# Output folder
GAPBS_RUN_DIR="$RUNDIR/gapbs"

# GAPBS path, needed by cmd.sh scripts
export GAPBS_DIR="/root/gapbs/gapbs"
export GAPBS_GRAPH_DIR="/root/gapbs/gapbs/benchmark/graphs/"
[[ ! -d "${GAPBS_DIR}" ]] && echo "${GAPBS_DIR} does not exist!" && exit
[[ ! -d "${GAPBS_GRAPH_DIR}" ]] && echo "${GAPBS_GRAPH_DIR} does not exist!" && exit


if [[ ! -e /usr/bin/time ]]; then
    echo "Please install GNU time first!"
    exit
fi

if [[ $# != 1 ]]; then
    echo ""
    echo "$0 <workload-file>"
    echo "  e.g. $0 w.txt => run all the workload in w.txt"
    echo ""
    exit
fi

wf=$1 # "w.txt"
LID=$2
[[ ! -e $wf ]] && echo "$wf doesnot exist .." && exit

run_seq()
{
    for id in 1 2 3 4 5; do
        cd "$warr"
        bash cmd.sh
        cd ..
    done
    echo "run_seq_base Done!"
}

main()
{
    # we deleted `pr-kron 18161`, `sssp-kron 35396`, `tc-kron 38368`, and `sssp-urand 37422`.
    # because it will crash with 8vcpus and 31G memory.
    for LID in $(seq 1 30); do
        warr=($(cat $wf | head -n $LID | tail -n 1 | grep -v "^#" | awk '{print $1}'))
        marr=($(cat $wf | head -n $LID | tail -n 1 | grep -v "^#" | awk '{print $2}'))
        echo "------------------- Workload:" $warr "-------------------"
        run_seq
    done
}

main

# ssh-keygen -R $VM_IP
# ssh -o "StrictHostKeyChecking no" root@10.10.1.100