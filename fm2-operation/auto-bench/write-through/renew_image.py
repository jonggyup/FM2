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

if __name__ == "__main__":
    src = fabric.Connection(host = "src")
    client = fabric.Connection(host = "dst")

    run_sync(src, "/mnt/mynvm/qemu-study/kernel-image/", "sudo bash create-image.sh")
    run_sync(src, "/mnt/mynvm/qemu-study/", "ssh-keygen -R 10.10.1.100")

# python3 spark_write_through.py normal vm_spark_normal.txt 0 > workload_spark_normal.txt