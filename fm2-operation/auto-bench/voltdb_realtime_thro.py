import re
import matplotlib.pyplot as plt
import numpy as np
import os
import sys

def extract_progress_throughput(text):
    pattern = r"(\d+\.\d+)%.*?at (\d+\.\d+) SP/sec"
    match = re.search(pattern, text)

    if match:
        progress = float(match.group(1)) * 200 / 100
        sp_per_sec = float(match.group(2))
        return (True, progress, sp_per_sec)
    
    return (False, False)

def parse_file(file_path):
    thro_lst = []
    x_ax = []
    with open(file_path, 'r') as file:
        for line in file:
            ret = extract_progress_throughput(line)
            if ret[0] == True:
                x_ax.append(float(ret[1]))
                thro_lst.append(float(ret[2]))
    return x_ax, thro_lst

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python script.py <file_path>")
        sys.exit(1)
    
    plt.figure(figsize=(40, 24))  # Width = 10 inches, Height = 6 inches

    files = [sys.argv[i] for i in range(1, len(sys.argv))]
    for file_path in files:
        x_ax, thro_lst = parse_file(file_path)
        plt.plot(x_ax, thro_lst, marker='o', label=file_path)
    
    plt.legend()
    plt.title("VoltDB + TPC-C")
    plt.xlabel("time(seconds)")
    plt.ylabel("tps")
    plt.ylim(bottom=0)

    xticks = [i * 2 for i in range(int(x_ax[-1]) // 2)]  # You can customize this range
    xticks = xticks[::3]
    plt.xticks(xticks)  # Set x-ticks
    # plt.savefig('rate_control.jpg', dpi=500)
    plt.show()