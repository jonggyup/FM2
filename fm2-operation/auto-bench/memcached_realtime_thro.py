import re
import matplotlib.pyplot as plt
import sys

# Function to extract "27 sec" and "current ops/sec"
def extract_data(line):
    pattern = r"(\d+)\s+sec:.*?(\d+(?:\.\d+)?)\s+current ops/sec"
    match = re.search(pattern, line)
    if match:
        return int(match.group(1)), float(match.group(2))  # sec, current ops/sec
    return None, None

def parse_result_file(file_path):
    time_sec = []
    ops_per_sec = []

    with open(file_path, 'r') as file:
        for line in file:
            sec, ops = extract_data(line)
            if sec is not None and ops is not None:
                time_sec.append(sec)
                ops_per_sec.append(ops)

    return time_sec, ops_per_sec

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python script.py <file_path>")
        sys.exit(1)

    plt.figure(figsize=(20, 12))

    files = [sys.argv[i] for i in range(1, len(sys.argv))]
    for file_path in files:
        time_sec, ops_per_sec = parse_result_file(file_path)

        if time_sec and ops_per_sec:
            plt.plot(time_sec, ops_per_sec, marker='o', label=file_path)
        else:
            print("No data found in the file.")
    
    plt.legend()
    plt.title('Memcached throughput over time')
    plt.xlabel('Time (seconds)')
    plt.ylabel('Ops/sec')
    plt.ylim(bottom=0)
    # plt.grid(True)
    xticks = [i * 2 for i in range(int(time_sec[-1]) // 2)]  # You can customize this range
    xticks = xticks[::3]
    plt.xticks(xticks)  # Set x-ticks
    plt.show()