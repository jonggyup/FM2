# FM2

FM2 is a QEMU/KVM prototype for fast live VM migration through memory shared
between source and destination hosts. It targets a multi-headed CXL memory
device and minimizes service interruption by maintaining a current VM image in
shared memory before switchover.

This repository is the artifact for **“FM2: Fast Live VM Migration in CXL
Memory Pools,”** accepted at ACM SIGOPS ATC 2026. The included functional
evaluation can run without the original CXL testbed by using Device DAX on a
multisocket NUMA machine.

## Overview

FM2 divides migration into three operations:

- **FMSync** continuously copies dirty guest-memory regions from source DRAM
  into shared CXL memory while the VM is running. Modified KVM provides coarse
  dirty and accessed-bit information with low tracking overhead.
- **FMFlush** briefly stops the source, copies the final 4 KiB-granularity
  changes and VM state, publishes a consistent shared image, and commits
  flattened hotness metadata.
- **FMLift** resumes the destination directly from CXL-backed memory, then
  promotes memory asynchronously into destination DRAM. Hotter 2 MiB regions
  are promoted first when valid metadata is available.

```text
 source VM in local DRAM
          |
          | FMSync: continuous dirty-memory copies
          v
 shared CXL memory image
          |
          | FMFlush: final state and ownership handoff
          v
 destination VM on CXL memory
          |
          | FMLift: hot-first background promotion
          v
 destination VM in local DRAM
```

## Repository layout

```text
FM2/
├── fm2-kernel/       Linux/KVM dirty and accessed-bit tracking extensions
├── fm2-qemu/         FMSync, FMFlush, restore, metadata, and FMLift
├── fm2-operation/    setup, controllers, VM images, workloads, and extractors
├── README.md         project overview and quick start
├── FUNCTIONALITY.md  detailed implementation and evaluation manual
└── LICENSE
```

The main source-code relationship is:

```text
fm2-kernel KVM ioctls
        |
        v
fm2-qemu source migration path ---- shared memory ---- destination restore
        |                                                |
        +-- FMSync/FMFlush metadata                       +-- FMLift
                              ^
                              |
                 fm2-operation controllers
```

`fm2-kernel` tracks guest dirty state for copying and accessed state for
approximate hotness. `fm2-qemu` implements the migration protocol and shared
image. `fm2-operation` supplies the deployment and validation harness; it is
not part of the core FM2 mechanism.

## Software versions

| Component | Version |
| --- | --- |
| FM2 host kernel | Linux `6.8.0` |
| Guest VM kernel | Linux `5.15.0` |
| FM2 QEMU | QEMU `8.2.91` |

These are the identifiers reported by the included source trees. The host must
boot the FM2 kernel, while the guest image uses the separate Linux 5.15 kernel.

## Hardware configurations

The paper design uses separate source and destination machines connected to a
shared CXL device. The current `fm2-operation` configuration emulates that
shared device with a reserved `/dev/dax0.0` range on one multisocket host:

```text
source QEMU on NUMA node A ─┐
                            ├── /dev/dax0.0
destination QEMU on node B ─┘
```

This setup validates FM2's mechanisms, handoff, application availability, and
result generation. It does not reproduce absolute performance results from the
original CXL hardware.

## Requirements

- x86-64 Linux with hardware virtualization and KVM;
- root access for kernel installation, Device DAX, QEMU, tap networking, and
  guest-image creation;
- a multisocket or multi-NUMA-node host for the supplied CXL substitute;
- sufficient reservable memory for `/dev/dax0.0`, plus local memory for both
  QEMU processes;
- sufficient disk space for kernel builds and the sparse guest image;
- Internet access for the initial dependency, guest-image, YCSB, and VoltDB
  setup; and
- a dedicated evaluation host, because the harness changes networking and
  terminates workload/QEMU processes during cleanup.

## Configuration

Machine-specific settings are in `fm2-operation/config.txt`. At minimum,
review:

- `SHARED_STORAGE`;
- `SRC_NUMA`, `DST_NUMA`, and `CXL_NUMA`;
- `SRC_CPUSET` and `DST_CPUSET`;
- `NIC_NAME` and the guest/controller addresses; and
- `CXL_DEV_PATH` and shared-memory sizing.

The source and destination CPU sets should reside on different sockets for the
functional CXL emulation. The current prototype expects `/dev/dax0.0` in parts
of QEMU, so changing only `CXL_DEV_PATH` may not be sufficient.

## Quick start

The complete commands, expected output, source-level checks, and evaluation
guidelines are in [FUNCTIONALITY.md](FUNCTIONALITY.md). The normal workflow is:

1. Configure, build, and install `fm2-kernel` using
   `fm2-kernel/kernel_setup.sh`.
2. Run `fm2-operation/setup_devdax.sh`, then reboot into the FM2 kernel.
3. Build and install `fm2-qemu`.
4. Run `fm2-operation/setup_init.sh` to install dependencies, build workloads,
   prepare the guest kernel/image, and compile the controller.
5. Run `fm2-operation/setup_network.sh` and verify Device DAX with
   `setup_devdax.sh --check`.
6. Run `run-redis-fm2.sh` or `run-voltdb-fm2.sh`.
7. Convert the generated logs to CSV with the matching `extract_*.sh` scripts.

See [Section 11: Build and environment setup](FUNCTIONALITY.md#11-build-and-environment-setup)
for preparation and [Section 12: Evaluation procedure](FUNCTIONALITY.md#12-evaluation-procedure)
for the evaluator-facing run and result-extraction commands.

## Workloads and outputs

The functional harness provides Redis with YCSB workloads A-F and VoltDB with
TPC-C. It records:

- application throughput in `fm2_*_perf_*.dat`;
- migration and downtime measurements in `fm2_*_downtime_*.dat`;
- source and destination QEMU evidence in `vm_src*.txt` and `vm_dst*.txt`; and
- evaluator-readable CSV files produced by the extraction scripts.

Successful functional validation requires background synchronization, a
completed switchover, destination resume, FMLift progress, and application
throughput after handoff. Exact timing and throughput are machine-dependent.

## Documentation

- [FUNCTIONALITY.md](FUNCTIONALITY.md): detailed execution flow, function and
  command maps, build/setup procedure, evaluation checks, and troubleshooting.

## Questions

For questions about FM2 or the artifact, contact Jonggyu Park at
[jonggyu@cs.washington.edu](mailto:jonggyu@cs.washington.edu) or
[jonggyu.pk@gmail.com](mailto:jonggyu.pk@gmail.com).

## License

See [LICENSE](LICENSE). The repository includes modified Linux and QEMU source
trees whose individual files may carry their own licensing notices.
