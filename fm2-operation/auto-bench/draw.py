import matplotlib.pyplot as plt
import numpy as np
import os



def print_lat(tmp, latency, l, r, mark):
    print("asd123www", l, r)
    lat_migration = [y for x,y in zip(tmp, latency) if x >= l and x < r]
    lat_migration.sort()
    # print(len(lat_migration), int(0.5 * len(lat_migration)))
    print(mark, "median lat:", lat_migration[int(0.5 * len(lat_migration))], "99th lat: ", lat_migration[int(0.99 * len(lat_migration))])
    if mark == "During migration":
        print(sorted(latency, reverse=True)[:40])

def latency_dot_graph(direc, filename, THREASHOLD):
    print(direc + filename)
    # read data.
    time = []
    latency = []
    left = 0
    right = 0
    flag = False
    # Open the file for reading
    with open(direc + filename, 'r') as file:
        # Iterate over each line in the file
        for line in file:
            # Split the line into parts using space (or another delimiter) as the separator
            parts = line.split()
            
            # Convert the parts to floats and add them as a tuple to the data list
            # This assumes each line correctly contains two float numbers
            if len(parts) == 2:
                print(line)
                flag = True
                left = float(parts[0])
                right = float(parts[1])
            else:
                assert(len(parts) == 3)
                time.append(float(parts[0]))
                latency.append(float(parts[1]) / 1000.0)


    start_time = time[0]
    left = (left - start_time) / 1000000000.0
    right = (right - start_time) / 1000000000.0
    tmp = [(t - start_time) / 1000000000.0 for t in time]

    interval_x = (start_time - time[0]) / 1000000000.0
    interval_y = 10.0

    if flag == False:
        left = tmp[0]
        right = tmp[-1]
        print_lat(tmp, latency, interval_x, interval_y, "No migration")
    else:
        print_lat(tmp, latency, interval_x, left, "Before migration")
        print_lat(tmp, latency, left, right, "During migration")
        print_lat(tmp, latency, right, interval_y, "After migration")

    print()
    print()


    print("Total time:", max(tmp))
    idx_lst  = [i for i, t in enumerate(tmp) if t >= interval_x and t < interval_y]
    time = [tmp[i] for i in idx_lst]
    latency = [latency[i] for i in idx_lst] # latency[:len(time)]
    # time = np.linspace(min(), 100, 500)  # Simulating time from 1 to 100 seconds
    # latency = np.random.normal(loc=100, scale=20, size=500)  # Generating random latencies

    print(left, right)

    plt.figure(figsize=(20, 12))

    fig, (ax1, ax2) = plt.subplots(2, 1, sharex=True)
    fig.subplots_adjust(hspace=0.05)  # adjust space between axes

    x_top_list = [x for x, y in zip(time, latency) if y >= THREASHOLD]
    y_top_list = [y for y in latency if y >= THREASHOLD]

    def print_with_color(ax, vecx, vecy):
        vecx1 = [x for x, y in zip(vecx, vecy) if x >= left and x <= right]
        vecy1 = [y for x, y in zip(vecx, vecy) if x >= left and x <= right]
        vecx2 = [x for x, y in zip(vecx, vecy) if x < left or x > right]
        vecy2 = [y for x, y in zip(vecx, vecy) if x < left or x > right]
        ax.scatter(vecx1, vecy1, color='orange', s = 1)
        ax.scatter(vecx2, vecy2, color='blue', s = 1)

    print_with_color(ax1, x_top_list, y_top_list)
    print_with_color(ax2, time, latency)
    # ax1.scatter(x_top_list, y_top_list, label='Latency', color='blue', s = 5)
    # ax2.scatter(time, latency, label='Latency', color='blue', s = 5)

    # plot the same data on both axes
    ax1.set_ylim(THREASHOLD, (3) * 1000000)
    # ax1.set_ylim(THREASHOLD, (max(latency) // 1000000 + 1) * 1000000)  # outliers only
    ax2.set_ylim(0, THREASHOLD)  # most of the data

    ax2.set_ylabel('Latency (us)')
    ax2.set_xlabel('Time (seconds)')

    # Now, let's add a break mark on the y-axis to indicate the discontinuity
    d = .015  # size of the diagonal lines in axes coordinates
    kwargs = dict(transform=ax1.transAxes, color='k', clip_on=False)
    ax1.plot((-d, +d), (-d, +d), **kwargs)        # top-left diagonal
    ax1.plot((1 - d, 1 + d), (-d, +d), **kwargs)  # top-right diagonal

    kwargs.update(transform=ax2.transAxes)  # switch to the bottom axes
    ax2.plot((-d, +d), (1 - d, 1 + d), **kwargs)  # bottom-left diagonal
    ax2.plot((1 - d, 1 + d), (1 - d, 1 + d), **kwargs)  # bottom-right diagonal

    # plt.scatter(time, latency, label='Latency', color='blue')
    # # plt.plot(time, latency, label='Latency', color='blue')
    # plt.xlabel('Time (seconds)')
    # plt.ylabel('Latency (us)')
    plt.legend()
    ax1.grid(True)
    ax2.grid(True)

    plt.savefig(direc + filename + '_graph.jpg', dpi=1000)

    # plt.show()
    print("\n")


def draw_frequency_control_ceiling_graph(points, ceiling):
    x, y = zip(*points)

    # Calculate percentages relative to 44000
    percentage = [val / ceiling * 100 for val in y]

    # Create the figure and axis
    plt.figure(figsize=(10, 6))

    # Plot the original data points
    plt.plot(x, y, marker='o', label='Data Points')

    # Plot the parallel line at y = 44000
    plt.axhline(y=ceiling, color='r', linestyle='--', label='without migration')

    # Annotate each point with its percentage
    for i, (xi, yi) in enumerate(points):
        plt.text(xi, yi, f"{percentage[i]:.1f}%", ha='left', va='bottom', fontsize=13)

    # Set labels and title
    plt.xlabel('Sleep time in milliseconds')
    plt.ylabel('Throughput')
    plt.title('Memcached')

    # plt.ylim(35000, 45000)

    # Add a legend
    plt.legend()

    # Show the plot
    plt.grid(True)
    plt.show()



if __name__ == "__main__":
    voltdb_normal_thro = 44011.37
    voltdb_points = [(1, 36257.43), (50, 40462.93), (100, 41482.28), (200, 42172.21), (300, 42534.67), (500, 43095.40), (1000, 42928.70)]

    redis_normal_thro = 410972.7855580558
    redis_points = [(1, 360178.6163858265), (50, 394311.88866804313), (100, 399354.4675592487), (200, 400513.8822532985), (300, 402469.7689249142), (500, 404037.54722305003)]

    memcached_normal_thro = 370747.9840578367
    memcached_points = [(1, 317218.627077782), (50, 357794.55436688254), (100, 358879.5779576163), (200, 362345.09747083124), (300, 363834.81899217755), (400, 363431.52041576564), (500, 363517.3943073176)]
    draw_frequency_control_ceiling_graph(memcached_points, memcached_normal_thro)


    direc = "tmp/"
    txt_files = [f for f in os.listdir(direc) if f.endswith('.txt')]
    # latency_dot_graph(direc, "post_copy_preemption_1.txt", THREASHOLD=1000)
    for filename in txt_files:
        if filename != "redis_result.txt": continue
        # if filename != "shm_local3.txt" and filename != "shm_local2.txt" and filename != "shm_local1.txt": continue
        latency_dot_graph(direc, filename, THREASHOLD=600)