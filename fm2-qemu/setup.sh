#!/bin/bash
rm ./build -rf
./configure --target-list=x86_64-softmmu --extra-cflags='-mavx2 -Wno-unused-variable -Wno-unused-function' --enable-kvm --enable-slirp
make -j 32
sudo make install
