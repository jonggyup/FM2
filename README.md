# FM2 Artifact

FM2 is a QEMU/KVM prototype for live VM migration through a shared CXL
memory pool. This checkout contains the source needed for the artifact's
Availability and basic Functionality evaluation. Reproducing the paper's
performance results requires the original two-host CXL testbed, which is not
currently available.

## Source layout

- `fm2-kernel/` — Linux 6.8-based KVM changes for FM2's huge-page and
  base-page FMSync dirty-log interfaces.
- `fm2-qemu/` — QEMU 8.2.91 changes for shared-memory migration, FMSync,
  FMFlush, FMLift promotion, userfaultfd handling, and FM2 HMP commands.

The detailed paper-to-source mapping is in
[`FM2_FUNCTIONALITY.md`](FM2_FUNCTIONALITY.md). It explains the functionality,
the relevant source locations, the QEMU–kernel call path, and the differences
from the upstream Linux/QEMU implementations.

## Hardware and configuration requirements

The intended setup is:

- two hosts connected to the same multi-headed CXL memory device;
- the shared CXL pool exposed as DevDAX on both hosts;
- Ethernet connectivity between the source and destination hosts;
- Ubuntu 22.04-class host and guest environments, with the FM2 host kernel
  based on Linux 6.8 and the guest image using Linux 5.15.

The current prototype is not device-path portable. The following values are
hardcoded or assumed in the source and scripts:

- `fm2-qemu/migration/migration-hmp-cmds.c` opens `/dev/dax1.0` for the
  destination-side shared-memory setup and `/dev/dax1.2` in the source-side
  migration path.
- `fm2-qemu/util/mmap-alloc.c` also contains a `/dev/dax1.0` fallback.

On a different testbed, update these paths, device names, offsets, IPs, NUMA
nodes, and remote working directories before running the scripts. A single
ordinary CXL device or a local file-backed mapping is not an equivalent
replacement for the paper's two-headed shared CXL setup.

## Build

Build the host kernel from its source directory:

```sh
cd fm2-kernel
make defconfig                 # or provide a testbed-specific .config
make -j"$(nproc)"
```

Build QEMU separately:

```sh
cd ../fm2-qemu
./configure --target-list=x86_64-softmmu
make -j"$(nproc)"
```
