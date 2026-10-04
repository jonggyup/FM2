#!/bin/bash
echo always | sudo tee /sys/kernel/mm/transparent_hugepage/shmem_enabled
sudo mount -t tmpfs -o remount,rw,nosuid,nodev,huge=always tmpfs /dev/shm
