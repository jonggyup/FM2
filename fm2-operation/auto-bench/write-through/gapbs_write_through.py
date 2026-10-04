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


def bench(mode, vm_path, duration):
    vcpus = 8
    memory = "32G"

    src_command = f"./apps/controller shm src apps/vm-boot/boot_vm.exp {vcpus} {memory} {vm_path} {duration}"

    print(src_command)
    ret1 = run_async(src, "/mnt/mynvm/qemu-study", src_command)
    sleep(80)
    run_sync(src, "/mnt/mynvm/qemu-study", "sudo bash scripts/pin_vm_to_cores.sh src")
    sleep(3)

    if mode == "shm":
        run_sync(src, "/mnt/mynvm/qemu-study", f"echo \"shm_migrate /my_shared_memory 33 {duration}\" | sudo socat stdio unix-connect:qemu-monitor-migration-src")

    ssh_command = f"""
        ssh -o "StrictHostKeyChecking no" root@10.10.1.100 << 'ENDSSH'
        echo "Connected to second server"
        # Place your commands here
        cd gapbs/script/
        bash run-gapbs.sh w.txt
        exit
        ENDSSH
        """
    
    run_sync(src, "~", ssh_command)
    

if __name__ == "__main__":
    src = fabric.Connection(host = "src")
    client = fabric.Connection(host = "dst")
    
    if len(sys.argv) != 4:
        print("Usage: python script.py <mode> <vm_output_path> <duration(us)>")
        sys.exit(1)
    
    mode = sys.argv[1]
    vm_path = sys.argv[2]
    duration = sys.argv[3]

    bench(mode, vm_path, duration)

# python3 gapbs_write_through.py normal vm_gapbs_normal.txt 0 > workload_gapbs_output.txt
