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

def bench(mode, duration):
    write_through_duration = 100 + 6

    src_command = f"./apps/controller {mode} src apps/vm-boot/voltdb.exp 4 16G vm_src.txt {duration} > ctl_src.txt"
    dst_command = f"./apps/controller {mode} dst 4 16G vm_dst.txt > ctl_dst.txt"
    backup_command = f"./apps/controller {mode} backup {write_through_duration} > ctl_backup.txt"

    print(src_command)
    ret1 = run_async(src, root_dir, src_command)
    ret2 = run_async(dst, root_dir, dst_command)
    ret3 = run_async(backup, root_dir, backup_command)
    sleep(80)

    client_init_command = "sudo bash apps/workload_scripts/voltdb/load_tpcc.sh"
    client_run_command = f"sudo bash apps/workload_scripts/voltdb/run_tpcc.sh 200 100 1000 > voltdb_client.txt"

    # warmup.
    run_sync(client, root_dir, client_init_command)
    run_sync(client, root_dir, "sudo bash apps/workload_scripts/voltdb/run_tpcc.sh 200 100 1000")

    ssh_command = f"""
        ssh -o "StrictHostKeyChecking no" root@10.10.1.100 << 'ENDSSH'
        echo "Connected to second server"
        # Place your commands here
        cd ~/voltdb/tests/test_apps/tpcc/
        kill -9 $(ps -eaf | grep voltdb | grep -v grep | awk '{{print $2}}')
        nohup bash run.sh > voltdb_log.out 2>&1 &
        sleep 5
        exit
        ENDSSH
        """

    run_sync(src, "~", ssh_command)
    run_sync(client, root_dir, client_init_command)
    run_sync(backup, root_dir, "sudo kill -SIGUSR1 $(cat controller.pid)")
    sleep(5)
    run_sync(src, root_dir, "sudo bash scripts/pin_vm_to_cores.sh src")
    run_sync(client, root_dir, client_run_command)

    run_sync(src, root_dir, "sudo bash scripts/my_kill.sh")
    run_sync(dst, root_dir, "sudo bash scripts/my_kill.sh")
    run_sync(backup, root_dir, "sudo bash scripts/my_kill.sh")

if __name__ == "__main__":
    src = fabric.Connection(host = "src")
    dst = fabric.Connection(host = "src")
    backup = fabric.Connection(host = "src")
    client = fabric.Connection(host = "dst")

    if len(sys.argv) != 4:
        print("Usage: python script.py <mode:`shm` or `qemu-precopy`> <duration(us)> <output_dir>")
        sys.exit(1)

    mode = sys.argv[1]
    duration = sys.argv[2]
    directory = sys.argv[3]

    bench(mode, duration)

    if not os.path.exists(directory):
        os.makedirs(directory)
    
    src.get(f"{root_dir}/vm_src.txt", f"{directory}/vm_src.txt")
    src.get(f"{root_dir}/ctl_src.txt", f"{directory}/ctl_src.txt")
    dst.get(f"{root_dir}/vm_dst.txt", f"{directory}/vm_dst.txt")
    dst.get(f"{root_dir}/ctl_dst.txt", f"{directory}/ctl_dst.txt")
    backup.get(f"{root_dir}/ctl_backup.txt", f"{directory}/ctl_backup.txt")
    client.get(f"{root_dir}/voltdb_client.txt", f"{directory}/voltdb_client.txt")

# For voltdb, the sleep interval is 500ms. So just go with `500000`.
# python3 voltdb.py shm 500000 voltdb-migrate/voltdb_test0