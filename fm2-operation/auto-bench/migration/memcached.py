import os
import sys
import fabric
from time import sleep

def run_async(node, path, command):
    with node.cd(path):
        return node.run(command, asynchronous = True, pty = True) # non-blocking
def run_sync(node, path, command):
    with node.cd(path):
        node.run(command, asynchronous = False, pty = True)

root_dir = "/mnt/mynvm/qemu-study"

def bench(mode, duration, workload):
    vcpus = 4
    memory = "16G"
    recordcount = 7500000
    operationcount = 80000000
    write_through_duration = 100 + 10

    src_command = f"./apps/controller {mode} src apps/vm-boot/memcached.exp {vcpus} {memory} vm_src.txt {duration} > ctl_src.txt"
    dst_command = f"./apps/controller {mode} dst {vcpus} {memory} vm_dst.txt > ctl_dst.txt"
    backup_command = f"./apps/controller {mode} backup {write_through_duration} > ctl_backup.txt"

    print(src_command)
    ret1 = run_async(src, root_dir, src_command)
    ret2 = run_async(dst, root_dir, dst_command)
    ret3 = run_async(backup, root_dir, backup_command)
    sleep(80)
    run_sync(src, root_dir, "sudo bash scripts/pin_vm_to_cores.sh src")
    sleep(3)

    client_init_command = f"sudo bash apps/workload_scripts/memcached/load_ycsb.sh {workload} 32 {recordcount}"
    client_run_command = f"sudo taskset -c 0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64 bash apps/workload_scripts/memcached/run_ycsb.sh {workload} 32 {recordcount} {operationcount} > memcached_client.txt"

    # warmup.
    run_sync(client, root_dir, client_init_command)
    run_sync(client, root_dir, f"sudo bash apps/workload_scripts/memcached/run_ycsb.sh {workload} 32 {recordcount} {operationcount}")

    # benchmark run, we should pin cores after the migration thread is actived, otherwise the migration thread will share the same core with one thread.
    run_sync(backup, root_dir, "sudo kill -SIGUSR1 $(cat controller.pid)")
    sleep(3)

    run_sync(client, root_dir, client_run_command)

    run_sync(src, root_dir, "sudo bash scripts/my_kill.sh")
    run_sync(dst, root_dir, "sudo bash scripts/my_kill.sh")
    run_sync(backup, root_dir, "sudo bash scripts/my_kill.sh")

if __name__ == "__main__":
    src = fabric.Connection(host = "src")
    dst = fabric.Connection(host = "src")
    backup = fabric.Connection(host = "src")
    client = fabric.Connection(host = "dst")

    if len(sys.argv) != 5:
        print("Usage: python script.py <mode:`shm` or `qemu-precopy`> <duration(us)> <output_dir> <workload>")
        sys.exit(1)

    mode = sys.argv[1]
    duration = sys.argv[2]
    directory = sys.argv[3]
    workload = sys.argv[4]

    bench(mode, duration, workload)

    if not os.path.exists(directory):
        os.makedirs(directory)

    src.get(f"{root_dir}/vm_src.txt", f"{directory}/vm_src.txt")
    src.get(f"{root_dir}/ctl_src.txt", f"{directory}/ctl_src.txt")
    dst.get(f"{root_dir}/vm_dst.txt", f"{directory}/vm_dst.txt")
    dst.get(f"{root_dir}/ctl_dst.txt", f"{directory}/ctl_dst.txt")
    backup.get(f"{root_dir}/ctl_backup.txt", f"{directory}/ctl_backup.txt")
    client.get(f"{root_dir}/memcached_client.txt", f"{directory}/memcached_client.txt")
    
# For memcached, the sleep interval is 500ms. So just go with `500000`