from distutils.log import warn
from http import server
from multiprocessing import Process
import pty
from pydoc import cli
from sys import stdout
from threading import Thread
from time import sleep
import sys
import os
import re



def redis_parser(file_path):
    file_lst = []
    result_lst = []
    for filename in sorted(os.listdir(dir_path)):
        if filename.endswith(".txt"):
            file_path = os.path.join(dir_path, filename)
            if "redis" in file_path:
                file_lst.append(file_path)
    
    pattern = r"\[OVERALL\], Throughput\(ops/sec\), ([\d.]+)"
    for file_path in file_lst:
        with open(file_path, 'r', errors='ignore') as file:
            for line_num, line in enumerate(file, 1):
                # Search for the pattern in the line
                match = re.search(pattern, line)
                if match:
                    throughput = match.group(1)
                    result_lst.append((file_path, throughput))
    result_lst.sort()

    group = 5
    for i in range(0, len(result_lst), group):
        print(result_lst[i: i + group])
        print("Median is: ", sorted([e[1] for e in result_lst[i: i + group]]))
        print()

    return

def memcached_parser(file_path):
    file_lst = []
    result_lst = []
    for filename in sorted(os.listdir(dir_path)):
        if filename.endswith(".txt"):
            file_path = os.path.join(dir_path, filename)
            if "memcached" in file_path:
                file_lst.append(file_path)
    
    pattern = r"\[OVERALL\], Throughput\(ops/sec\), ([\d.]+)"
    for file_path in file_lst:
        with open(file_path, 'r', errors='ignore') as file:
            for line_num, line in enumerate(file, 1):
                # Search for the pattern in the line
                match = re.search(pattern, line)
                if match:
                    throughput = match.group(1)
                    result_lst.append((file_path, throughput))
    result_lst.sort()

    group = 5
    for i in range(0, len(result_lst), group):
        print(result_lst[i: i + group])
        print("Median is: ", sorted([e[1] for e in result_lst[i: i + group]]))
        print()

    return

def voltdb_parser(file_path):
    file_lst = []
    result_lst = []
    for filename in sorted(os.listdir(dir_path)):
        if filename.endswith(".txt"):
            file_path = os.path.join(dir_path, filename)
            if "voltdb" in file_path:
                file_lst.append(file_path)
    
    pattern = r"Transactions per second: ([\d.]+)"
    for file_path in file_lst:
        with open(file_path, 'r', errors='ignore') as file:
            for line_num, line in enumerate(file, 1):
                # Search for the pattern in the line
                match = re.search(pattern, line)
                if match:
                    throughput = match.group(1)
                    print(f"{file_path}: in line {line_num} the throughput is {throughput}")
    return

def spark_parser(file_path):
    print("asd123www", file_path)
    results = []
    current_app = None

    def analyse_app(results):
        print(results[0][0])
        thro = [item[1] for item in results]
        thro.sort()
        print(thro)

    with open(file_path, 'r') as file:
        lines = file.readlines()

    for i, line in enumerate(lines):
        if "Throughput(bytes/s)" in line:
            data = re.split(r'\s+', lines[i + 1])
            throughput = data[5]
            current_app = data[0]
            if len(results) != 0 and results[-1][0] != current_app:
                analyse_app(results)
                results = []
            results.append((current_app, float(throughput)))
    analyse_app(results)

def parser(dir_path, apps):
    if not os.path.exists(dir_path):
        print(f"Directory {dir_path} does not exist.")
        return
    
    for app in apps:
        globals()[app + "_parser"](dir_path)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python script.py <directory>")
        sys.exit(1)
    
    dir_path = sys.argv[1]
    print(f"Parsing {dir_path} experiment resuls")
    spark_parser(dir_path)
    parser(dir_path, ["voltdb"])
    # parser(dir_path, ["redis", "memcached", "voltdb", "nginx", "gapbs", "spark"])