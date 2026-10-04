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

ROOT_PATH="/mnt/mynvm/qemu-study"

def bench(mode, vm_path, clt_path, duration, workload):
    vcpus = 4
    memory = "20G"
    recordcount = 5000000 # 9.75G
    operationcount = 10000000
    if workload == "workloade":
        operationcount = 30000
    server_num = 1
    thread_count = 6
    load_thread_count = 10

    src_command = f"./apps/controller shm src apps/vm-boot/redis.exp {vcpus} {memory} {vm_path} {duration}"
    client_init_command = f"sudo bash apps/workload_scripts/redis/load_ycsb.sh {workload} {server_num} {recordcount} {load_thread_count}"
    client_run_command = f"sudo bash apps/workload_scripts/redis/run_ycsb.sh {workload} {server_num} {thread_count} {recordcount} {operationcount} > {clt_path}"

    print(src_command)
    ret1 = run_async(src, ROOT_PATH, src_command)
    sleep(80)

    if mode == "shm":
        run_sync(src, ROOT_PATH, f"echo \"shm_migrate /my_shared_memory 21 {duration}\" | sudo socat stdio unix-connect:qemu-monitor-migration-src")
    sleep(3)

    # when adding pin cores, the results are not stable. But I don't know why.
    run_sync(src, ROOT_PATH, "sudo bash scripts/pin_vm_to_cores.sh src")
    run_sync(src, ROOT_PATH, "sudo bash scripts/pin_vhost.sh")
    sleep(3)

    # warmup.
    run_sync(client, ROOT_PATH, client_init_command)

    # experimental result.
    run_sync(client, ROOT_PATH, client_run_command)
    run_sync(src, ROOT_PATH, "sudo dmesg -C")


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

# python3 redis_write_through.py shm vm_redis_shm.txt clt_redis_shm.txt 1000 workloada
# python3 redis_write_through.py normal vm_redis_normal.txt clt_redis_normal.txt 0 workloada