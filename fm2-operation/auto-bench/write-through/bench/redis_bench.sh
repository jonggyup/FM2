num_iterations=5

workloads=(workloada workloadb workloadc workloadd workloade workloadf)

for workload in "${workloads[@]}"
do
    # without migration
    for i in $(seq 0 $((num_iterations - 1)))
    do
        echo "Running iteration: $i"

        # Dynamically change the file names with the current iteration index
        vm_file="vm_redis_${workload}_normal_${i}.txt"
        clt_file="clt_redis_${workload}_normal_${i}.txt"

        # Run the python script with the current files and sleep duration of 0
        python3 redis_write_through.py normal "redis-write-through/$vm_file" "redis-write-through/$clt_file" 0 $workload
    done

    # with different sleep time(frequency control).
    durations=(500000)
    for duration in "${durations[@]}"
    do
        ms_duration=$((duration / 1000))

        echo "Running with sleep duration: $duration us"

        for i in $(seq 0 $((num_iterations - 1)))
        do
            # Dynamically change the file names with the appropriate ms value
            vm_file="vm_redis_${workload}_shm_${ms_duration}ms_${i}.txt"
            clt_file="clt_redis_${workload}_shm_${ms_duration}ms_${i}.txt"

            python3 redis_write_through.py shm "redis-write-through/$vm_file" "redis-write-through/$clt_file" $duration $workload
        done
    done
done