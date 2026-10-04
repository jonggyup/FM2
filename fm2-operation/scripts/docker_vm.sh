#!/usr/bin/env bash
set -euo pipefail
source config.txt

# Number of parallel make jobs
JOBS=${JOBS:-$(nproc)}

tar xvf linux-$KERNEL_VER.tar.xz

cd linux-"$KERNEL_VER"

echo $PWD

echo ">> Base config: defconfig + KVM guest tweaks"
make defconfig
make kvm_guest.config

echo ">> Enabling Docker‐required features"
# KVM guest
scripts/config --enable CONFIG_KVM_GUEST
scripts/config --enable CONFIG_HAVE_KVM
scripts/config --enable CONFIG_PTP_1588_CLOCK_KVM

# Control groups
scripts/config --enable CONFIG_CGROUPS
scripts/config --enable CONFIG_CGROUP_CPUACCT
scripts/config --enable CONFIG_CGROUP_DEVICE
scripts/config --enable CONFIG_CGROUP_FREEZER
scripts/config --enable CONFIG_CGROUP_MEMCG
scripts/config --enable CONFIG_CGROUP_PERF

# BPF
scripts/config --enable CONFIG_BPF_SYSCALL
scripts/config --enable CONFIG_CGROUP_BPF
scripts/config --enable CONFIG_BPF_CGROUP_DEVICE

scripts/config --enable CONFIG_VETH

# Namespaces
scripts/config --enable CONFIG_NAMESPACES
scripts/config --enable CONFIG_NETNS
scripts/config --enable CONFIG_PID_NS
scripts/config --enable CONFIG_UTS_NS
scripts/config --enable CONFIG_IPC_NS

# OverlayFS & FUSE
scripts/config --enable CONFIG_OVERLAY_FS
scripts/config --enable CONFIG_FUSE_FS

# Networking: bridge + netfilter
scripts/config --enable CONFIG_BRIDGE
scripts/config --enable CONFIG_BRIDGE_NETFILTER
scripts/config --enable CONFIG_NETFILTER_XT_MATCH_ADDRTYPE
scripts/config --enable CONFIG_NF_CONNTRACK
scripts/config --enable CONFIG_NF_CONNTRACK_NETLINK
scripts/config --enable CONFIG_NF_CONNTRACK_IPV4
scripts/config --enable CONFIG_NF_NAT
scripts/config --enable CONFIG_NF_NAT_IPV4
scripts/config --enable CONFIG_IP_NF_TARGET_MASQUERADE
scripts/config --enable CONFIG_IP_NF_FILTER
scripts/config --enable CONFIG_NF_TABLES
scripts/config --enable CONFIG_IP_NF_NAT
scripts/config --enable CONFIG_IP_NF_RAW
scripts/config --enable CONFIG_NF_TABLES_INET

# Filesystem watchers (inotify, optional but nice)
scripts/config --enable CONFIG_INOTIFY_USER

echo ">> Re-generate .config with new options"
make olddefconfig

echo ">> Building kernel and modules with $JOBS jobs"
make -j"$JOBS"

echo ">> Build complete!"
echo "Your bzImage is at: $(pwd)/arch/$(uname -m)/boot/bzImage"
echo "Install modules with: sudo make modules_install"
echo "Install kernel with: sudo cp arch/$(uname -m)/boot/bzImage /boot/vmlinuz-custom-$KERNEL_VER && sudo update-grub"


# To install docker in debian: "https://docs.docker.com/engine/install/debian/".
# You need to switch to legacy iptables and ip6tables: "https://wiki.debian.org/iptables".