num_iterations=5

workloads=(workloada workloadb workloadc workloadd workloade workloadf)

for workload in "${workloads[@]}"
do
    # without migration
    for i in $(seq 0 $((num_iterations - 1)))
    do
        echo "Running iteration: $i"

        # Dynamically change the file names with the current iteration index
        vm_file="vm_memcached_${workload}_normal_${i}.txt"
        clt_file="clt_memcached_${workload}_normal_${i}.txt"

        # Run the python script with the current files and sleep duration of 0
        python3 memcached_write_through.py normal "memcached-write-through/$vm_file" "memcached-write-through/$clt_file" 0 $workload
    done

    # with different sleep time(frequency control).
    # durations=(1000 50000 100000 200000 300000 400000 500000)
    durations=(500000)
    for duration in "${durations[@]}"
    do
        ms_duration=$((duration / 1000))

        echo "Running with sleep duration: $duration us"

        for i in $(seq 0 $((num_iterations - 1)))
        do
            # Dynamically change the file names with the appropriate ms value
            vm_file="vm_memcached_shm_${workload}_${ms_duration}ms_${i}.txt"
            clt_file="clt_memcached_shm_${workload}_${ms_duration}ms_${i}.txt"

            python3 memcached_write_through.py shm "memcached-write-through/$vm_file" "memcached-write-through/$clt_file" $duration $workload
        done
    done
done