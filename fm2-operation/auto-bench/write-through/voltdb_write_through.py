from distutils.log import warn
from http import server
from multiprocessing import Process
import pty
from pydoc import cli
from sys import stdout
from threading import Thread
from time import sleep
from unittest import result
import threading
import fabric
import invoke
import sys
# pip install Fabric

def run_async(node, path, command):
    with node.cd(path):
        return node.run(command, asynchronous = True, pty = True) # non-blocking

def run_sync(node, path, command):
    with node.cd(path):
        node.run(command, asynchronous = False, pty = True)


def bench(mode, vm_path, clt_path, duration):
    src_command = f"./apps/controller shm src apps/vm-boot/voltdb.exp 4 16G {vm_path} {duration}"
    client_init_command = "sudo bash apps/workload_scripts/voltdb/load_tpcc.sh"
    client_run_command = f"sudo bash apps/workload_scripts/voltdb/run_tpcc.sh 200 100 1000 > {clt_path}"

    print(src_command)
    ret1 = run_async(src, "/mnt/mynvm/qemu-study", src_command)
    sleep(80)
    if mode == "shm":
        run_sync(src, "/mnt/mynvm/qemu-study", f"echo \"shm_migrate /my_shared_memory 17 {duration}\" | sudo socat stdio unix-connect:qemu-monitor-migration-src")
    sleep(3)
    
    run_sync(src, "/mnt/mynvm/qemu-study", "sudo bash scripts/pin_vm_to_cores.sh src")
    sleep(3)

    # warmup.
    run_sync(client, "/mnt/mynvm/qemu-study/", client_init_command)
    run_sync(client, "/mnt/mynvm/qemu-study/", "sudo bash apps/workload_scripts/voltdb/run_tpcc.sh 200 100 1000")

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
    run_sync(client, "/mnt/mynvm/qemu-study/", client_init_command)
    run_sync(client, "/mnt/mynvm/qemu-study/", client_run_command)


if __name__ == "__main__":
    src = fabric.Connection(host = "src")
    client = fabric.Connection(host = "dst")
    
    if len(sys.argv) != 5:
        print("Usage: python script.py <mode> <vm_output_path> <client_output_path> <duration(us)>")
        sys.exit(1)
    
    mode = sys.argv[1]
    vm_path = sys.argv[2]
    clt_path = sys.argv[3]
    duration = sys.argv[4]

    bench(mode, vm_path, clt_path, duration)

# python3 voltdb_write_through.py normal vm_voltdb_normal.txt clt_voltdb_normal.txt 0
# python3 voltdb_write_through.py shm vm_voltdb_shm.txt clt_voltdb_shm.txt 1000