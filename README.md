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

All commands in this section are run from the repository root unless noted
otherwise.

### Prerequisites

Install the required dependencies on Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential bc bison flex libssl-dev libelf-dev \
    libncurses-dev dwarves python3 python3-venv python3-pip \
    python3-setuptools ninja-build pkg-config libglib2.0-dev \
    libpixman-1-dev libnuma-dev
```

#### Toolchain requirements

The sources track Linux 6.8 and QEMU 8.2.91, which target an Ubuntu 22.04-class
toolchain (GCC 11/12). Building with **GCC 15 or newer** fails unless the extra
flags described under [Newer toolchains](#newer-toolchains-gcc-15-or-newer) are
supplied. Check your compiler with:

```bash
gcc --version
```

### Build and Install Host Kernel

```bash
cd fm2-kernel
make defconfig                 # or provide a testbed-specific .config
make -j"$(nproc)"
sudo make modules_install
sudo make install
sudo update-grub
sudo reboot
```

After reboot, verify that the FM2 kernel is running:

```bash
uname -r
```

### Build QEMU

From the repository root (after rebooting, change back into this checkout
first):

```bash
cd fm2-qemu
./configure --target-list=x86_64-softmmu --extra-cflags="-mavx"
make -j"$(nproc)"
```

The FM2 QEMU binary is generated at `fm2-qemu/build/qemu-system-x86_64`.

Verify the build with:

```bash
./build/qemu-system-x86_64 --version
```

### Newer toolchains (GCC 15 or newer)

On distributions that ship GCC 15 or newer (for example Ubuntu 26.04, verified
with GCC 15.2), the verbatim commands above fail in two upstream components.
Neither failure is in FM2 code, and both are avoided with extra build flags — no
source changes are required.

**Kernel — EFI boot stub.** `drivers/firmware/efi/libstub/Makefile` replaces
rather than extends `KBUILD_CFLAGS` on x86, so it loses the `-std=gnu11` set by
the top-level `Makefile`. GCC 15 defaults to `-std=gnu23`, where `bool`,
`true`, and `false` are reserved keywords, and the build stops with:

```text
./include/linux/stddef.h:11:9: error: cannot use keyword 'false' as enumeration constant
./include/linux/types.h:35:33: error: 'bool' cannot be defined via 'typedef'
```

Force the expected C standard on the compiler itself (note that `KCFLAGS` does
*not* work here, because the EFI stub discards `KBUILD_CFLAGS`):

```bash
cd fm2-kernel
make defconfig
make -j"$(nproc)" CC="gcc -std=gnu11"
```

**QEMU — bundled dtc/libfdt.** The vendored device-tree subproject builds with
`-Werror`, and GCC 15 tracks `const` through `memchr()` more strictly:

```text
subprojects/dtc/libfdt/fdt_overlay.c:461:21: error: assignment discards 'const'
    qualifier from pointer target type [-Werror=discarded-qualifiers]
```

Relax that single diagnostic at configure time:

```bash
cd fm2-qemu
./configure --target-list=x86_64-softmmu \
    --extra-cflags="-mavx -Wno-error=discarded-qualifiers"
make -j"$(nproc)"
```

Alternatively, install a matching compiler with `sudo apt install -y gcc-12`,
then build the kernel with `make CC=gcc-12` and configure QEMU with
`--cc=gcc-12`.

