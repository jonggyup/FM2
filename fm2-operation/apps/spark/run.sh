# Must run with `apps/vm-boot/port_fwd.exp` for internet.
#
# We can evaluate the write-through's overhead, but can't do end-to-end migration test.
# 

if [[ $# != 1 ]]; then
    echo ""
    echo "$0 <workload-file>"
    echo "  e.g. $0 w.txt => run all the workload in w.txt"
    echo ""
    exit
fi

wf=$1 # "w.txt"
[[ ! -e $wf ]] && echo "$wf doesnot exist .." && exit

run_seq()
{
    for id in 1 2 3 4 5; do
        cd "$warr"
        bash prepare_exp.sh
        bash run_exp.sh
        cd ..
    done
    echo "run_seq_base Done!"
}

main()
{
    for LID in $(seq 1 11); do
        warr=($(cat $wf | head -n $LID | tail -n 1 | grep -v "^#" | awk '{print $1}'))
        echo "------------------- Workload:" $warr "-------------------"
        run_seq
    done
}

main