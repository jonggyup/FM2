sudo apt update
sudo apt install cmake libtbb-dev -y

cd YCSB-C-master
make clean
make

# load libredis++.so.1
sudo ldconfig