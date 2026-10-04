## vm-boot directory 

This directory has all machine booting scripts. There are four inputs: `kernel_dir`: the kernel you compiled, we will load `arch/x86_64/boot/bzImage`. `driver_dir`: the output `.img` from `kernel-image`, this path points to the shared storage. `cpu_num` and `memory_size` specify the VM size. One example to run is `./scripts/vm-boot/bfs.exp ./linux-5.10.54 /proj/xdp-PG0 4 9G`.

If you want to run different benchmarks, you should execute different scripts. For example, for the `bfs` benchmark, you should execute `bfs.exp`.