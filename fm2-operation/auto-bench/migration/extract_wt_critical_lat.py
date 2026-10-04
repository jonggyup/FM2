import os
import re

def extract_pre_copy_duration(file_content):
    # Regex pattern to extract the "pre-copy duration" followed by a number
    pattern = r"pre-copy duration:\s*(\d+)\s*ns"
    match = re.search(pattern, file_content)
    if match:
        return int(match.group(1))  # Extract the number
    return None

def parse_ctl_backup(directory):
    file_path = os.path.join(directory, "ctl_backup.txt")
    if os.path.isfile(file_path):
        with open(file_path, 'r') as file:
            content = file.read()
        duration = extract_pre_copy_duration(content)
        duration = float(duration) / 1000000
        if duration:
            print(f"Directory: {directory}, VM remove critical path duration: {duration:.3f} ms")
        else:
            print(f"Directory: {directory}, 'pre-copy duration' not found.")
    else:
        print(f"Directory: {directory}, 'ctl_backup.txt' not found.")

def enumerate_directories(base_dir):
    for root, dirs, files in os.walk(base_dir):
        for dir_name in dirs:
            full_dir_path = os.path.join(root, dir_name)
            parse_ctl_backup(full_dir_path)

if __name__ == "__main__":
    base_directory = input("Enter the base directory path: ")
    enumerate_directories(base_directory)
