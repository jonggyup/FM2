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


def bench(mode, vm_path, clt_path, duration, workload):
    vcpus = 4
    memory = "16G"
    recordcount = 7500000 # 10.5G memory.
    operationcount = 80000000 # 50000000

    src_command = f"./apps/controller shm src apps/vm-boot/memcached.exp {vcpus} {memory} {vm_path} {duration}"
    client_init_command = f"sudo bash apps/workload_scripts/memcached/load_ycsb.sh {workload} 32 {recordcount}"
    client_run_command = f"sudo taskset -c 0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64 bash apps/workload_scripts/memcached/run_ycsb.sh {workload} 32 {recordcount} {operationcount} > {clt_path}"

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
    run_sync(client, "/mnt/mynvm/qemu-study/", f"sudo bash apps/workload_scripts/memcached/run_ycsb.sh {workload} 32 {recordcount} {operationcount}")

    # experimental result.
    run_sync(client, "/mnt/mynvm/qemu-study/", client_run_command)


if __name__ == "__main__":
    src = fabric.Connection(host = "src")
    client = fabric.Connection(host = "dst")
    
    if len(sys.argv) != 6:
        print("Usage: python script.py <mode> <vm_output_path> <client_output_path> <duration(us)> <workload>")
        sys.exit(1)
    
    mode = sys.argv[1]
    vm_path = sys.argv[2]
    clt_path = sys.argv[3]
    duration = sys.argv[4]
    workload = sys.argv[5]

    bench(mode, vm_path, clt_path, duration, workload)

# python3 memcached_write_through.py normal vm_memcached_normal.txt clt_memcached_normal.txt 0 workloada
# python3 memcached_write_through.py shm vm_memcached_shm.txt clt_memcached_shm.txt 1000 workloada