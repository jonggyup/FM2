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
    vcpus = 4
    memory = "15G"
    wrk_thread = 1
    wrk_connections = 120
    benchmark_time = 100

    src_command = f"./apps/controller shm src apps/vm-boot/nginx.exp {vcpus} {memory} {vm_path} {duration}"
    client_run_command = f"sudo bash apps/workload_scripts/nginx/run.sh {wrk_thread} {wrk_connections} {benchmark_time} > {clt_path}"

    print(src_command)
    ret1 = run_async(src, "/mnt/mynvm/qemu-study", src_command)
    sleep(80)
    run_sync(src, "/mnt/mynvm/qemu-study", "sudo bash scripts/pin_vm_to_cores.sh src")
    sleep(3)

    if mode == "shm":
        run_sync(src, "/mnt/mynvm/qemu-study", f"echo \"shm_migrate /my_shared_memory 16 {duration}\" | sudo socat stdio unix-connect:qemu-monitor-migration-src")

    # warmup.
    run_sync(client, "/mnt/mynvm/qemu-study/", f"sudo bash apps/workload_scripts/nginx/run.sh {wrk_thread} {wrk_connections} {benchmark_time}")

    # experimental result.
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

# python3 nginx_write_through.py shm vm_nginx_shm.txt clt_nginx_shm.txt 1000
# python3 nginx_write_through.py normal vm_nginx_normal.txt clt_nginx_normal.txt 0