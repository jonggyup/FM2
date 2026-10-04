# update the kernel image
source config.txt

if [ -z "$1" ]; then
  echo "No program defined"
  exit 1
else
  echo "VM will run program: $1"
fi

if [ -z "$2" ]; then
  echo "CPU number not defined"
  exit 1
else
  echo "# of vCPUs: $2"
fi

if [ -z "$3" ]; then
  echo "Memory size not defined"
  exit 1
else
  echo "Memory size: $3"
fi


# cd kernel-image
# chmod +x create-image.sh
# sudo bash create-image.sh
# rm -f $SHARED_STORAGE/bullseye.img
# cp bullseye.img $SHARED_STORAGE
# cd ..

# start the VM
$1  ./linux-$KERNEL_VER $SHARED_STORAGE/kernel-image $2 $3 $VM_IP $SRC_CPUSET