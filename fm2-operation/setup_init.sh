#!/bin/bash

source config.txt

sudo apt update
sudo apt --fix-broken install -y
sudo apt install openjdk-8-jdk -y
sudo apt install maven -y

# Build application artifacts only when they are missing. These outputs are
# git-ignored, so a fresh artifact checkout cannot rely on them being present.
if ! compgen -G 'apps/ycsb/redis/target/ycsb-redis-binding-*.tar.gz' >/dev/null ||
   ! compgen -G 'apps/ycsb/memcached/target/ycsb-memcached-binding-*.tar.gz' >/dev/null; then
    cd apps/ycsb
    sudo mvn -pl site.ycsb:redis-binding -am clean package
    sudo mvn -pl site.ycsb:memcached-binding -am clean package
    cd ../..
fi

# build gapbs
cd apps/gapbs/gapbs
make
# make bench-graphs // for full graph generation
cd ../../..

if [[ ! -f apps/voltdb/voltdb/voltdb/voltdb-11.4.0.beta1.jar ]]; then
    cd apps/voltdb
    sudo apt install docker.io -y
    sudo docker build --network=host . -t voltdb
    sudo docker rm -f voltdb-run >/dev/null 2>&1 || true
    sudo docker create --name voltdb-run voltdb
    if [[ -d voltdb ]]; then
        sudo docker cp voltdb-run:/opt/voltdb/voltdb/. "$PWD/voltdb/voltdb/"
    else
        sudo docker cp voltdb-run:/opt/voltdb .
    fi
    sudo docker rm voltdb-run
    cd ../..
fi

if ! command -v wrk >/dev/null 2>&1; then
    sudo apt install lua-socket luarocks -y
    sudo luarocks install luasocket
    cd apps/wrk
    sudo make -j 10
    sudo cp wrk /usr/local/bin
    cd ../..
fi

# qemu dependency.
sudo apt-get install linux-generic libelf-dev socat redis-server redis libboost-all-dev pip -y
sudo apt install git libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev python3-venv ninja-build flex bison debootstrap -y

# for linux kernel compile
sudo apt install -y build-essential bc bison flex libssl-dev libelf-dev \
    libncurses-dev dwarves python3 python3-venv python3-pip \
    python3-setuptools ninja-build pkg-config libglib2.0-dev \
    libpixman-1-dev libnuma-dev


# recommended.
# sudo apt-get install git-email -y
sudo apt-get install libaio-dev libbluetooth-dev libcapstone-dev libbrlapi-dev libbz2-dev -y
sudo apt-get install libcap-ng-dev libcurl4-gnutls-dev libgtk-3-dev -y
sudo apt-get install libibverbs-dev libjpeg8-dev libncurses5-dev libnuma-dev -y
sudo apt-get install librbd-dev librdmacm-dev libsasl2-dev libsdl2-dev libseccomp-dev libsnappy-dev libssh-dev -y
sudo apt-get install libvde-dev libvdeplug-dev libvte-2.91-dev libxen-dev liblzo2-dev valgrind xfslibs-dev libnfs-dev libiscsi-dev expect -y

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
cd kernel-image
./create-image.sh


# initialize apps and controller.
cd apps
gcc controller.c -o controller -O3
cd redis
sudo bash setup_redis_client.sh
cd ../../
