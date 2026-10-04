#!/bin/bash

# Check for argument: `qemu` is vanilla qemu, `main` is shm migration.
if [ -z "$1" ]; then
  echo "No QEMU branch was provided: \`qemu\` or \`main\`"
  exit 1
else
  echo "QEMU branch: $1"
fi

source config.txt

git config --global --add safe.directory '*'
sudo git submodule init
sudo git submodule update

sudo apt update
sudo apt --fix-broken install -y
sudo apt install openjdk-8-jdk -y
sudo apt install maven -y

# build ycsb
cd apps/ycsb
sudo mvn -pl site.ycsb:redis-binding -am clean package
sudo mvn -pl site.ycsb:memcached-binding -am clean package
cd ../..

# build gapbs
cd apps/gapbs/gapbs
make
# make bench-graphs
cd ../../..

# build voltdb
cd apps/voltdb
sudo apt install docker.io -y
sudo docker build --network=host . -t voltdb
sudo docker rm -f voltdb-run >/dev/null 2>&1 || true
sudo docker create --name voltdb-run voltdb
if [ -d voltdb ]; then
  sudo docker cp voltdb-run:/opt/voltdb/voltdb/. "$PWD/voltdb/voltdb/"
else
  sudo docker cp voltdb-run:/opt/voltdb .
fi
sudo docker rm voltdb-run
cd ../..

# build wrk
sudo apt install lua-socket luarocks -y
sudo luarocks install luasocket
cd apps/wrk
sudo make -j 10
sudo cp wrk /usr/local/bin
cd ../..

# qemu dependency.
sudo apt-get install linux-generic libelf-dev socat redis-server redis libboost-all-dev pip -y
sudo apt install git libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev python3-venv ninja-build flex bison debootstrap -y

# recommended.
# sudo apt-get install git-email -y
sudo apt-get install libaio-dev libbluetooth-dev libcapstone-dev libbrlapi-dev libbz2-dev -y
sudo apt-get install libcap-ng-dev libcurl4-gnutls-dev libgtk-3-dev -y
sudo apt-get install libibverbs-dev libjpeg8-dev libncurses5-dev libnuma-dev -y
sudo apt-get install librbd-dev librdmacm-dev libsasl2-dev libsdl2-dev libseccomp-dev libsnappy-dev libssh-dev -y
sudo apt-get install libvde-dev libvdeplug-dev libvte-2.91-dev libxen-dev liblzo2-dev valgrind xfslibs-dev libnfs-dev libiscsi-dev expect -y

cd apps/stress-ng
git checkout tags/V0.16.00
# sudo make -j 4
# sudo make install
cd ../..

# check kvm support.
if [ $(egrep -c '(vmx|svm)' /proc/cpuinfo) -gt 0 ]; then
    echo "KVM is supported"
else 
    echo "KVM is supported"
    exit 1
fi

if lsmod | grep kvm; then 
    echo "KVM module loaded"
else 
    # sudo modprobe kvm-intel
    # sudo modprobe kvm-amd
    echo "KVM module not loaded"
fi

cd qemu-master
git checkout $1
./configure --target-list=x86_64-softmmu --enable-kvm --enable-slirp
make -j 10
sudo make install
cd ..

sudo apt install libslirp0 -y

# compile Linux code.
echo "Use Linux-$KERNEL_VER"
wget https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-$KERNEL_VER.tar.xz
tar xvf linux-$KERNEL_VER.tar.xz
cd linux-$KERNEL_VER
make defconfig
make kvm_guest.config
CONFIG_KVM_GUEST=y
CONFIG_HAVE_KVM=y
CONFIG_PTP_1588_CLOCK_KVM=y
make olddefconfig
./scripts/config -e MEMCG
make -j 10
cd ..

# creating an image for the kernelPermalink.
sudo apt-get install debootstrap
# cd kernel-image
# chmod +x create-image.sh
# sudo ./create-image.sh

# initialize apps.
cd apps
gcc controller.c -o controller -O3
cd redis
sudo bash setup_redis_client.sh

# Save current IPv4/CIDR and default gateway
IP_CIDR=$(ip -4 -o addr show "$NIC" | awk '{print $4}')
GW=$(ip route show default | awk '{print $3}')

# Create bridge (idempotent)
sudo ip link add name br0 type bridge 2>/dev/null || true
sudo ip link set br0 up

# Move NIC into bridge
sudo ip addr flush dev "$NIC"
sudo ip link set "$NIC" master br0
sudo ip link set "$NIC" up

# Re-attach IP and default route to bridge
sudo ip addr add "$IP_CIDR" dev br0
sudo ip route add default via "$GW" dev br0

# Create multi-queue tap devices and attach to bridge
for tap in tap0 tap1; do
    sudo ip tuntap add dev "$tap" mode tap multi_queue
    sudo ip link set "$tap" up
    sudo ip link set "$tap" master br0
done

# disable nic adaptive batching.
# sudo ethtool -C $NIC_NAME adaptive-rx off adaptive-tx off rx-frames 1 rx-usecs 0  tx-frames 1 tx-usecs 0
# sudo ethtool -C $NIC_NAME adaptive-rx off adaptive-tx off rx-frames 1 rx-usecs 0  tx-frames 1 tx-usecs 0
# sudo ethtool -C tap0 adaptive-rx off adaptive-tx off rx-frames 1 rx-usecs 0  tx-frames 1 tx-usecs 0
# sudo ethtool -C tap1 adaptive-rx off adaptive-tx off rx-frames 1 rx-usecs 0  tx-frames 1 tx-usecs 0
