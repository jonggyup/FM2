# FM2 Functionality, Implementation Map, and Evaluation Procedure

This document describes how FM2 works, where each mechanism is implemented,
how `fm2-operation` drives it, and what an artifact evaluator should inspect to
award the Functional badge. Paths are relative to the `FM2/` repository root.

## 1. Scope of the functional evaluation

FM2 maintains a current VM-memory image in shared CXL memory, stops the source
only for a final consistency pass, resumes the destination directly from the
shared image, and moves the active working set back to local DRAM after resume.
The three named mechanisms are:

- **FMSync:** background dirty-memory synchronization and hotness collection;
- **FMFlush:** the final stop-and-copy consistency boundary and handoff;
- **FMLift:** hot-first, fault-safe promotion from CXL-backed memory to local
  DRAM.

The artifact has two evaluation levels:

1. **Source/build validation**, which confirms that the kernel ioctls, QEMU HMP
   controls, and modified binaries exist.
2. **End-to-end functional validation**, which requires KVM and Device DAX and
   demonstrates migration while Redis or VoltDB remains a live service.

The included harness runs source and destination QEMU on one NUMA machine and
uses one Device DAX mapping as the common pool. The paper architecture places
the roles on different hosts attached to a multi-headed CXL device. Therefore,
the harness establishes functionality but is not, by itself, a reproduction
of the paper's absolute performance results.

## 2. Components and responsibilities

```text
fm2-operation/run-*-fm2.sh
  |
  +-- apps/controller (three processes)
  |     +-- source role: starts source VM and commands FMSync/FMFlush
  |     +-- destination role: prepares incoming VM and commands FMLift
  |     `-- backup role: coordinates the handoff and records timestamps
  |
  +-- apps/vm-boot/*.exp
  |     +-- source QEMU: FM2_IS_SOURCE_VM=1, tap0, local DRAM
  |     `-- destination QEMU: FM2_IS_SOURCE_VM=0, tap1, Device DAX
  |
  +-- fm2-qemu
  |     +-- FMSync loop and CXL shadow layout
  |     +-- FMFlush completion and cache publication
  |     `-- FMLift userfaultfd/mremap promotion
  |
  `-- fm2-kernel
        +-- KVM huge-page/base-page FM2 dirty logging
        `-- Device-DAX userfaultfd write-protection support
```

The workload client is a host process. The Redis or VoltDB server runs in the
guest at `10.0.0.1`. Both QEMU instances use the same guest MAC address; only
the active instance should own the service at any time.

## 3. Configuration contract

`fm2-operation/config.txt` is read by the controller, boot scripts, workload
wrappers, and parts of QEMU. Commands must be launched from
`FM2/fm2-operation/` so relative lookup succeeds.

### 3.1 Path and guest configuration

| Variable | Consumer | Function |
| --- | --- | --- |
| `SHARED_STORAGE` | `scripts/src_boot.sh`, `scripts/dst_boot.sh` | Absolute operation directory containing `kernel-image/` |
| `KERNEL_VER` | boot scripts | Selects `linux-$KERNEL_VER/arch/x86_64/boot/bzImage` |
| `VM_IP` | source Expect launcher and clients | Active guest service address |
| `NIC_NAME` | legacy/setup scripts | Testbed NIC; `setup_network.sh` takes an environment override |

### 3.2 Controller configuration

| Variable | Function |
| --- | --- |
| `SRC_IP`, `DST_IP`, `BACKUP_IP` | Addresses used by controller roles; supplied harness uses loopback |
| `SRC_CONTROL_PORT` | Source-controller channel |
| `DST_CONTROL_PORT` | Destination-controller channel |
| `BACKUP_CONTROL_PORT` | Reserved backup-controller port |
| `MIGRATION_PORT` | Conventional QEMU migration mode port; FM2 shared-memory mode uses the control channels and HMP |

### 3.3 Placement and memory layout

| Variable | Function |
| --- | --- |
| `SRC_NUMA` | Local NUMA node for source guest RAM |
| `DST_NUMA` | Local NUMA node used for destination-local allocations |
| `CXL_NUMA` | NUMA identity assigned to the CXL/shared region |
| `SRC_CPUSET`, `DST_CPUSET` | Host CPUs passed to `taskset` for each QEMU |
| `MIGRATION_CORE`, `VHOST_CORE`, `NIC_INTERRUPT_CORE` | Testbed affinity controls used by optional helper scripts |
| `META_STATE_LENGTH` | 1 MiB QEMU state/control prefix in the shared mapping |
| `HOT_PAGE_STATE_LENGTH` | Hotness table following the metadata prefix |
| `CXL_BASE_OFF_BYTES` | Intended Device DAX base offset; active prototype paths currently use offset zero |

`CXL_DEV_PATH` documents the intended path, but the active source currently
opens `/dev/dax0.0` directly in QEMU. A path change therefore requires a source
edit and QEMU rebuild.

### 3.4 Runtime environment

The Expect launchers set `FM2_IS_SOURCE_VM=1` for the source and `0` for the
destination. `util/mmap-alloc.c` rejects an unset or invalid value because the
two roles require different RAM mappings.

`FM_MIG_COPY_THREADS` controls the parallel copy worker pool used during the
first switchover pass. The controller propagates additional legacy tuning
variables (`REC_HOT_NTHREADS`, `NTHREADS`, `REM_NTHREADS`, `CXLSHM_PATH`, and
`CXLSHM_NODE`); not all are consumed by the current QEMU data path.

## 4. Shared CXL layout

The active QEMU constants divide the shared mapping as follows:

```text
offset 0
  +-----------------------------+
  | serialized VM state/queue   | META_STATE_LENGTH (default 1 MiB)
  +-----------------------------+
  | FMSync hotness metadata     | HOT_PAGE_STATE_LENGTH (default 9 MiB)
  +-----------------------------+
  | guest RAM shadow            | per-RAMBlock data at pages_offset_shm
  +-----------------------------+
```

`migration/ram.c` defines `RAM_FILE_BASE` as the sum of the metadata and
hotness regions. The controller passes `VM memory size + 1 GiB` to the HMP
commands to leave room for migration state and alignment. The Device DAX
namespace must cover every mapping performed by both the migration HMP path
and destination RAM mapping path.

The HOT2 hotness region contains a 64-byte `Fm2HotHdr`, a 4 MiB open-addressed
hash used while FMSync is active, and a compact-list area occupying the
remainder. Fixed-size 16-byte `Fm2HotEntry` records identify 2 MiB-aligned file
offsets and retain last-epoch/frequency information. FMFlush flattens live hash
entries into the compact area and commits the header. FMLift validates the
magic, version, state, sizes, and bounds before using the list. If metadata is
absent, invalid, or uncommitted, FMLift falls back to address order.

## 5. End-to-end protocol

### 5.1 Initialization

1. `run-redis-fm2.sh` or `run-voltdb-fm2.sh` starts a source controller:

   ```text
   apps/controller shm src <expect-script> <vcpus> <memory> vm_src.txt <interval-us>
   ```

2. `scripts/src_boot.sh` invokes the selected Expect script with the guest
   kernel, guest disk, CPU count, memory size, guest IP, and source CPU set.
3. The Expect script starts QEMU with KVM, `FM2_IS_SOURCE_VM=1`, tap0, an HMP
   Unix socket named `qemu-monitor-migration-src`, and
   `-global kvm-apic.vapic=off`.
4. Source guest RAM is allocated in local DRAM and bound to `SRC_NUMA`.
5. The guest boots, obtains `10.0.0.1`, and starts Redis or VoltDB.
6. A destination controller waits for the backup/controller protocol before
   launching the incoming QEMU process.

### 5.2 FMSync background synchronization

The backup controller sends `shm_migrate` to the source role. The source role
issues:

```text
shm_migrate /my_shared_memory <memory-size-plus-one-GiB> <interval-us>
```

`shm_migrate` is the compatibility command used by the operation harness;
`migrate_FMSync_start` reaches the same implementation.

QEMU then performs:

```text
hmp_shm_migrate()
  -> map /dev/dax0.0
  -> qmp_shm_migrate()
       -> shm_init()
       -> shm_migration_thread()
            -> qemu_savevm_state_header_shm()
            -> migration_iteration_run_shm()
                 -> qemu_savevm_state_iterate_shm()
                      -> ram_save_iterate_shm()
```

On the first pass, `ram_save_iterate_shm()` copies all migratable RAMBlocks to
the CXL shadow. On later background passes it:

1. requests FM2 huge-page dirty state from KVM and, at most once per second,
   samples the hardware accessed bits;
2. walks one dirty bit per 2 MiB region;
3. copies dirty runs into the shadow using non-temporal CXL copy helpers;
4. clears the corresponding bitmap bits;
5. updates recency/frequency entries for dirty and accessed 2 MiB regions;
6. applies configured FMSync bandwidth/frequency pacing; and
7. logs `[MIG] iterate: switchover=0 ...`.

The source VM continues executing throughout these background iterations.
Accessed bits are advisory metadata only: they update hotness but never cause a
memory copy. Dirty bits remain the sole source of FMSync copy work. KVM clears
the sampled dirty/accessed state and flushes the affected translations, so a
region is reported in a later epoch only after it is dirtied/accessed again.

### 5.3 Destination preparation

Five seconds before the configured handoff point, the backup controller sends
`start_target_vm` to the destination controller. It launches
`apps/vm-boot/incoming.exp`, which starts QEMU with:

- `FM2_IS_SOURCE_VM=0`;
- the same vCPU count and memory size;
- `-incoming defer`;
- tap1 and the source guest's MAC address;
- HMP socket `qemu-monitor-migration-dst`.

The destination controller issues:

```text
migrate_incoming_shm_setup /my_shared_memory <memory-size-plus-one-GiB>
```

This maps and invalidates the shared Device DAX window so that the destination
can consume the image after FMFlush.

### 5.4 FMFlush and switchover

At the handoff point, the backup controller records `Migration start` and asks
the source controller to switchover. The source issues
`shm_migrate_switchover` (alias: `migrate_FMSync_switchover`).

The QEMU completion path is:

```text
qmp_shm_migrate_switchover()
  -> atomic_switchover = true
  -> migration_iteration_run_shm()
  -> migration_completion_shm()
       -> migration_completion_precopy_shm()
            -> stop source VM
            -> qemu_savevm_state_complete_precopy_shm()
                 -> ram_save_complete_shm()
```

The final pass changes from 2 MiB observation to 4 KiB dirty tracking using
the base-page KVM ioctl. Dense dirty 2 MiB windows can still be copied as a
whole; sparse windows are copied as 4 KiB runs. FMFlush fences non-temporal
stores, compacts the live source hash entries into a contiguous HOT2 list,
flushes that list, and commits `list_count`, `LIST_COMMITTED`, and a new
generation in the header only after the data is persistent. It then flushes
the remaining shared state and notifies the source controller that the image
is complete. This ordering prevents FMLift from accepting a partially
flattened table.

The backup controller measures downtime from the complete-image notification
until the destination restart notification. It records:

```text
pre-copy duration: <nanoseconds> ns
vm downtime: <nanoseconds> ns
Migration start: <monotonic nanoseconds> ns
Migration end: <monotonic nanoseconds> ns
```

### 5.5 Destination restore

After the source image is complete, the backup controller tells the destination
to load it. The destination controller issues:

```text
migrate_incoming_shm /my_shared_memory <memory-size-plus-one-GiB>
```

The QEMU path is:

```text
hmp_migrate_incoming_shm()
  -> qmp_migrate_incoming_shm()
       -> create QEMU input channel over shared metadata
       -> process_incoming_migration_shm_co()
            -> qemu_loadvm_state_shm()
            -> process_incoming_migration_bh()
```

The destination resumes the guest with its RAMBlocks backed by Device DAX and
signals the destination controller. The service retains the same guest IP and
MAC, so clients can continue or reconnect without changing endpoints.

### 5.6 FMLift promotion

After restore, the destination controller issues:

```text
migrate_promotion_shm /my_shared_memory <memory-size-plus-one-GiB>
```

`migrate_FMLift_start` is the paper-named alias. The active HMP handler locates
the `pc.ram` RAMBlock and calls `qemu_promote_vm_cxl_memory()`.

`migration/fm2_promoter.c` then:

1. identifies the Device-DAX-backed VM range;
2. creates a shadow mapping for the same Device DAX file/offset;
3. registers bounded windows with userfaultfd;
4. tracks each 2 MiB chunk as unpromoted, promoting, or promoted;
5. builds a hot-first order from FMSync metadata when it is valid;
6. write-protects a chunk and copies it into anonymous local DRAM;
7. atomically replaces the old mapping with `mremap(MREMAP_FIXED)`;
8. handles missing/write-protection faults that race with background workers;
9. rate-limits background promotion if configured; and
10. clears global promotion state after completion so a later run can start.

The VM executes while FMLift runs. A vCPU access to an unpromoted or in-flight
region is coordinated with userfaultfd so it does not observe a partially
replaced mapping.

For HOT2 metadata, FMLift accepts only the expected magic, version, entry size,
committed state, list bounds, and list count. It maps each valid shared-file
offset to a promotion chunk and sorts by hotness/recency before address order.
It retains read compatibility with the older HOT1 hash representation. Invalid
or unavailable metadata is rejected safely and promotion proceeds in address
order.

## 6. Kernel implementation map

### 6.1 Userspace API

[`fm2-kernel/include/uapi/linux/kvm.h`](fm2-kernel/include/uapi/linux/kvm.h)
defines:

- `KVM_FMSYNC_GET_DIRTY_LOG_HUGE`: return/clear one dirty bit per 2 MiB
  region during background FMSync, optionally returning a separate 2 MiB
  accessed bitmap through the extended FM2 log request;
- `KVM_FMSYNC_GET_DIRTY_LOG_BASE_WITH_SPLIT`: split huge mappings as needed
  and return/clear 4 KiB dirty state for FMFlush.

### 6.2 Generic KVM path

[`fm2-kernel/virt/kvm/kvm_main.c`](fm2-kernel/virt/kvm/kvm_main.c):

- allocates a private `fmsync_dirty_bitmap` per KVM memory slot;
- allocates a separate `fmsync_accessed_bitmap` for low-frequency hotness
  samples;
- validates the userspace slot/ioctl request;
- takes the MMU lock and invokes the architecture walker;
- flushes TLB state when permissions/dirty state are changed; and
- copies the resulting bitmap back to QEMU.

### 6.3 x86 TDP MMU path

[`fm2-kernel/arch/x86/kvm/mmu/tdp_mmu.c`](fm2-kernel/arch/x86/kvm/mmu/tdp_mmu.c)
walks TDP leaf SPTEs. Huge mode emits 2 MiB granularity. Base mode splits or
walks mappings at 4 KiB granularity. Dirty/write state is cleared so the next
epoch observes only subsequent guest writes.

[`fm2-kernel/arch/x86/kvm/mmu/mmu.c`](fm2-kernel/arch/x86/kvm/mmu/mmu.c) and
[`fm2-kernel/arch/x86/kvm/mmu/tdp_mmu.h`](fm2-kernel/arch/x86/kvm/mmu/tdp_mmu.h)
connect the generic ioctl path to the x86 implementation.

### 6.4 Device DAX userfaultfd support

[`fm2-kernel/include/linux/userfaultfd_k.h`](fm2-kernel/include/linux/userfaultfd_k.h)
allows the write-protection capability required for the FM2 DAX VMA.
[`fm2-kernel/mm/mprotect.c`](fm2-kernel/mm/mprotect.c) contains the corresponding
FM2 DAX protection path/diagnostics.

## 7. QEMU implementation map

| File/function | FM2 responsibility |
| --- | --- |
| `migration/migration.c:migration_iteration_run_shm()` | Continuous FMSync controller and pacing loop |
| `migration/migration.c:qmp_shm_migrate()` | Initialize shared target and migration thread |
| `migration/migration.c:qmp_shm_migrate_switchover()` | Atomically request FMFlush |
| `migration/migration.c:qmp_migrate_incoming_shm()` | Start destination restore from shared state |
| `migration/ram.c:ram_save_iterate_shm()` | Initial copy, huge dirty iterations, hotness updates, switchover pass |
| `migration/ram.c:ram_save_complete_shm()` | Final dirty drain, metadata publication, cache fences |
| `migration/ram.c:hot_flatten_region()` | Convert the live hotness hash into the committed HOT2 list |
| `migration/ram.c:fmsync_memory_dirty_log_huge()` | Issue FM2 KVM dirty-log ioctls |
| `migration/ram.c:fm2_fmsync_set_frequency()` | Update background interval |
| `migration/ram.c:fm2_fmsync_set_bandwidth()` | Update FMSync bandwidth cap |
| `migration/fm2_layout.h` | Shared metadata constants and HOT1/HOT2 on-media structures |
| `accel/kvm/kvm-all.c` | Allocate QEMU bitmaps and translate FM2 KVM dirty/accessed results into RAMBlocks |
| `migration/savevm.c` | Shared save/load header, setup, iterate, and completion hooks |
| `migration/migration-hmp-cmds.c` | User-facing HMP commands and Device DAX mappings |
| `migration/fm2_promoter.c:qemu_promote_vm_cxl_memory()` | Launch FMLift for the active guest RAM mapping |
| `migration/fm2_promoter.c:fm2_promoter_set_bandwidth()` | FMLift rate control |
| `util/mmap-alloc.c:mmap_activate()` | Role-dependent source/local versus destination/DAX RAM mapping |
| `hmp-commands.hx` | HMP command declarations/help |

## 8. HMP command map

| Paper-named command | Harness compatibility command | Role |
| --- | --- | --- |
| `migrate_FMSync_start URI SIZE [INTERVAL_US]` | `shm_migrate URI SIZE INTERVAL_US` | Map the shared target and start FMSync |
| `migrate_FMSync_freqctrl INTERVAL_US` | none | Set time between FMSync rounds; zero disables extra pacing |
| `migrate_FMSync_bandctrl BYTES_PER_SEC` | none | Cap background FMSync; zero disables the cap |
| `migrate_FMSync_switchover` | `shm_migrate_switchover` | Request FMFlush/handoff |
| `migrate_incoming_shm_setup URI SIZE` | same | Prepare destination shared mapping |
| `migrate_incoming_shm URI SIZE` | same | Restore VM state from the shared image |
| `migrate_FMLift_start URI SIZE` | `migrate_promotion_shm URI SIZE` | Start CXL-to-DRAM promotion |
| `migrate_FMLift_bandctrl BYTES_PER_SEC` | none | Cap background promotion; zero disables the cap |

The `URI` argument is retained for interface compatibility; the active Device
DAX implementation ignores it and opens the compiled device path.

## 9. Operation-script map

| Script | Inputs/effect | Evidence produced |
| --- | --- | --- |
| `fm2-kernel/kernel_setup.sh` | Derives the FM2 host-kernel configuration from the running kernel | `fm2-kernel/.config` |
| `setup_devdax.sh` | Reserves aligned RAM through GRUB and checks Device DAX | `/dev/dax0.0`, `daxctl` state |
| `setup_network.sh` | Creates `br-private`, tap0, tap1, and host `10.0.0.254/24` | `ip -br` state |
| `setup_init.sh` | Installs dependencies and builds missing YCSB/VoltDB/wrk artifacts, guest kernel/image, and controller | binaries and `bullseye.img` |
| `kernel-image/create-image.sh` | Debootstrap guest, copy apps, create sparse ext4 image | `kernel-image/bullseye.img` |
| `apps/controller` | Implements source/destination/backup state machines | PID and downtime logs |
| `scripts/src_boot.sh` | Starts workload-specific source launcher | `vm_src.txt` |
| `scripts/dst_boot.sh` | Starts deferred incoming destination launcher | `vm_dst.txt` |
| `apps/vm-boot/redis.exp` | Boots guest and starts per-vCPU Redis instances | guest service |
| `apps/vm-boot/voltdb.exp` | Boots guest and starts VoltDB TPC-C server | guest service |
| `run-redis-fm2.sh` | Runs YCSB A-F with an FM2 migration per workload | Redis `.dat` logs |
| `run-voltdb-fm2.sh` | Runs TPC-C with FM2 migration | VoltDB `.dat` logs |
| `extract_throughput_redis.sh` | Parses `current ops/sec` | elapsed/workload CSV |
| `extract_time_redis.sh` | Parses only `fm2_redis_downtime_*.dat` | downtime/migration CSV |
| `extract_throughput_voltdb.sh` | Converts `% Complete` and `SP/sec` using duration | elapsed/TPC-C CSV |
| `extract_time_voltdb.sh` | Parses VoltDB controller timing | downtime/migration CSV |
| `scripts/my_kill.sh` | Stops controllers, QEMU, and workload processes | clean testbed |

## 10. Workload behavior

### 10.1 Redis/YCSB

`run-redis-fm2.sh` iterates over A-F. For each workload it:

1. removes stale shared/controller state and screen sessions;
2. launches a 4-vCPU, 20 GiB source guest;
3. waits until `10.0.0.1` responds;
4. loads 5,000,000 YCSB records;
5. launches the destination and backup controller roles;
6. runs YCSB and records one-second throughput;
7. waits for YCSB's `Starting test.` marker and the configured warmup;
8. applies the FMSync bandwidth cap and signals the backup role, which starts
   FMSync and later FMFlush;
9. waits for post-handoff execution; and
10. terminates the processes before the next workload.

Workloads A/B/C/D/F run 20,000,000 operations; E runs 2,000,000. Redis is
started inside the VM; the YCSB client runs on the host. The default Jedis
timeout is 15 seconds, the default actual-workload warmup is 10 seconds, and
background FMSync is uncapped (`0`) and copies in paced 64 MiB chunks when a
cap is selected. Override these with
`REDIS_TIMEOUT_MS`, `YCSB_WARMUP_SECONDS`, and
`FM_FMSYNC_BANDWIDTH_BPS`. The migration thread is bound to
`MIGRATION_CORE`; affinity failure is reported without killing QEMU.

### 10.2 VoltDB/TPC-C

`run-voltdb-fm2.sh`:

1. launches a 4-vCPU, 20 GiB source guest and starts VoltDB;
2. launches destination and backup roles;
3. initializes TPC-C data;
4. starts a nominal 300-second, 100-warehouse TPC-C client with 1-second
   display interval;
5. signals migration after a warm-up period; and
6. records TPC-C `SP/sec` and controller timing.

The included VoltDB build treats a corrupt client frame as a per-connection
failure and enables client reconnect. This avoids taking down unrelated client
connections; it is not a substitute for correct memory migration.

## 11. Build and environment setup

The commands below are the supported functional-evaluation path. Begin in the
`FM2/` repository root. The setup requires root access and should run on a
dedicated x86-64 KVM host.

### 11.1 Configure the artifact

Edit `fm2-operation/config.txt` before launching any controller. Set
`SHARED_STORAGE` to the absolute path of `fm2-operation` and adapt the NUMA,
CPU-set, and NIC values to the host:

```bash
cd fm2-operation
sed -n '1,200p' config.txt
cd ..
```

Confirm that the requested CPU IDs belong to the intended nodes and that there
is enough contiguous memory for the Device-DAX reservation:

```bash
lscpu -e=CPU,NODE,SOCKET,ONLINE
numactl -H
grep 'System RAM' /proc/iomem
egrep -c '(vmx|svm)' /proc/cpuinfo
test -c /dev/kvm
```

### 11.2 Configure, build, and install the FM2 host kernel

Install normal kernel-build dependencies if the host does not already provide
them:

```bash
sudo apt update
sudo apt install -y build-essential bc bison flex libssl-dev libelf-dev \
  libncurses-dev dwarves rsync cpio fakeroot python3 ndctl daxctl numactl
```

`kernel_setup.sh` uses paths relative to the kernel tree, so run it from inside
`fm2-kernel`:

```bash
cd fm2-kernel
./kernel_setup.sh
make -j"$(nproc)"
sudo make modules_install
sudo make install
sudo update-grub
cd ..
```

Do not reboot yet. Configure the Device-DAX reservation first so the new kernel
and new kernel command line take effect together.

### 11.3 Reserve memory for Device DAX and reboot

The default reservation is 30 GiB from NUMA node 0. It must be larger than the
shared image required by the configured workload. To select another size/node,
pass them explicitly, for example `64G --node 1`.

```bash
cd fm2-operation
sudo ./setup_devdax.sh
# Example override: sudo ./setup_devdax.sh 64G --node 1
cd ..
sudo reboot
```
Here, make sure correctly selecting the newly compiled kernel when you reboot.

The script chooses an aligned System RAM range, adds a `memmap=` argument to
GRUB, and prints the selected range. Reserving an invalid physical range can
make the machine unbootable; the testbed administrator must review it before
rebooting.

After reboot:

```bash
uname -r
lsmod | grep '^kvm'
test -c /dev/kvm
```

`uname -r` must identify the newly installed FM2 kernel.

### 11.4 Build and install FM2 QEMU

```bash
sudo apt install -y git ninja-build pkg-config libglib2.0-dev \
  libpixman-1-dev libnuma-dev libfdt-dev zlib1g-dev libslirp-dev
cd fm2-qemu
mkdir -p build
cd build
../configure --target-list=x86_64-softmmu --enable-kvm \
  --enable-slirp --extra-cflags="-mavx" --disable-werror
ninja
sudo ninja install
cd ../..
```

Confirm that `PATH` resolves the modified executable rather than a distribution
QEMU:

```bash
command -v qemu-system-x86_64
qemu-system-x86_64 --version
strings "$(command -v qemu-system-x86_64)" | \
  grep -E 'migrate_FMSync_start|migrate_FMLift_start'
```

### 11.5 Install applications and build the guest environment

From the operation directory:

```bash
cd fm2-operation
./setup_init.sh
```

This script installs host/application dependencies, builds missing YCSB,
VoltDB, and `wrk` artifacts, builds the Linux 5.15 guest kernel, creates the
Debian guest disk image, configures the Redis client, and compiles
`apps/controller`. It downloads packages and is intentionally a heavyweight
first-run operation.


### 11.6 Configure the private VM network

`setup_network.sh` creates `br-private`, `tap0`, and `tap1`. The default bridge
address is `10.0.0.254/24`; the active guest uses `10.0.0.1/24`.

```bash
sudo ./setup_network.sh
```

For host-only networking, attach no physical NIC:

```bash
sudo env NIC_NAME="" ./setup_network.sh
```

Only attach a dedicated interface that has no host address or default route.
The script may reject an in-use management interface.

### 11.7 Verify Device DAX and final preconditions

```bash
sudo ./setup_devdax.sh --check
ls -l /dev/dax0.0
daxctl list
ip -br link show br-private tap0 tap1
ip -br -4 addr show br-private
```

Do not start an evaluation unless `/dev/dax0.0` exists, its capacity covers
the configured mapping, the FM2 kernel is running, and the modified QEMU is
selected through `PATH`.

## 12. Evaluation procedure

### 12.1 Preflight

Section 11 is the installation procedure; do not repeat kernel, Device-DAX, or
QEMU setup here. Start in `FM2/fm2-operation` and verify the installed
environment:

```bash
cd fm2-operation  # from the FM2 repository root
set -e
test -x ../fm2-qemu/build/qemu-system-x86_64
test -x ../fm2-kernel/vmlinux
test -x apps/controller
test -x linux-5.15/arch/x86_64/boot/bzImage
test -s kernel-image/bullseye.img
test -c /dev/kvm
test -c /dev/dax0.0
grep '^SHARED_STORAGE=' config.txt
numactl -H
ip -br link show br-private tap0 tap1
sudo ./setup_devdax.sh --check
```

Confirm that `PATH` selects FM2 QEMU:

```bash
command -v qemu-system-x86_64
qemu-system-x86_64 --version
strings "$(command -v qemu-system-x86_64)" | \
  grep -E 'migrate_FMSync_start|migrate_FMLift_start'
```

Do not run `enable_devdax.sh` as routine preflight. It destroys and recreates a
hard-coded namespace and is only a testbed-specific recovery tool.

### 12.2 Redis end-to-end run

```bash
sudo ./scripts/my_kill.sh
./apps/controller shm src apps/vm-boot/setup_redis.exp 4 16G vm_src.txt 400000 #wait until the installation is done (cat ./vm_src.txt shows the result)
./run-redis-fm2.sh
mkdir -p results
./extract_throughput_redis.sh -o results/redis_throughput.csv
./extract_time_redis.sh > results/redis_migration_times.csv
```

### 12.3 VoltDB end-to-end run

```bash
sudo ./scripts/my_kill.sh
./run-voltdb-fm2.sh
mkdir -p results
./extract_throughput_voltdb.sh --duration 300 \
  -o results/voltdb_throughput.csv
./extract_time_voltdb.sh -o results/voltdb_migration_times.csv
```

### 12.4 Mechanism evidence in logs

Redis retains per-workload logs; `vm_src.txt` and `vm_dst.txt` are copies of
the most recent run. Check all retained logs when evaluating the complete
suite:

```bash
grep -H -E '\[MIG\] iterate: switchover=0' vm_src*.txt
grep -H -E '\[MIG\] iterate: switchover=1' vm_src*.txt
grep -H -E 'in hmp_migrate_incoming_shm|shm load time' vm_dst*.txt
grep -H -E 'Starting promotion|hot 2 MiB regions prioritized|CXL promotion thread finished successfully' \
  vm_dst*.txt
```

### 12.5 Result interpretation

The timing scripts calculate:

```text
downtime_s  = vm downtime nanoseconds / 1e9
migration_s = (Migration end - Migration start) / 1e9
```

## 13. Failure diagnosis

### FM2 commands are missing

The launchers resolve `qemu-system-x86_64` through `PATH`. Reinstall FM2 QEMU
or put `fm2-qemu/build` first in `PATH`; distribution QEMU does not provide the
FM2 commands.

### `/dev/dax0.0` cannot be opened or mapped

Run `sudo ./setup_devdax.sh --check` and inspect `daxctl list`, permissions,
capacity, and the GRUB reservation. The active prototype contains paths that
expect `/dev/dax0.0`; changing only `config.txt` may not change them.

### The guest does not answer at `10.0.0.1`

Check `br-private`, tap0/tap1, `vm_src.txt`, and the Expect launcher. Only a
dedicated physical NIC without a host address/default route should be attached
to the bridge.

### A controller waits indefinitely

```bash
screen -ls
cat src_controller.pid dst_controller.pid controller.pid
tail -100 vm_src.txt
tail -100 vm_dst.txt
```

The controller protocol uses blocking reads, so a failed peer can leave
another role waiting for a notification.

### Promotion uses address order

Verify that source and destination use the same metadata sizes and shared
layout and that FMFlush committed the HOT2 list. Address-order promotion is a
safe fallback, but it is not evidence of hot-first FMLift.

### A workload log is incomplete

Do not make up missing timing or throughput values. Retain the raw file for
diagnosis, exclude it from completed-run extraction, and rerun that workload
for full-suite evidence.
