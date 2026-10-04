
python3 gapbs_write_through.py normal gapbs-write-through/vm_gapbs_normal.txt 0 > workload_gapbs_normal.txt

durations=(500000)
for duration in "${durations[@]}"
do
    ms_duration=$((duration / 1000))

    echo "Running with sleep duration: $duration us"

    vm_file="vm_gapbs_shm_${ms_duration}ms.txt"
    clt_file="workload_gapbs_shm_${ms_duration}ms.txt"

    python3 gapbs_write_through.py shm "gapbs-write-through/$vm_file" $duration > $clt_file
done
