# Applications

The applications running inside the migrated VM.

## Redis & Memcached

We run Redis server inside the VM and workload generator(ycsb) connecting over the network.  Redis is a single thread application. To use all available vCPUs in the VM, we create # of vCPUs Redis servers that each one is pinned to a vCPU core. You can check `.apps/vm-boot/redis.exp`. We also run memcached in the VM, with native multi-thread support. 

Check `apps/workload_scripts/redis` for workloads. Run load & run script under `.`. For example `sudo bash apps/workload_scripts/redis/run_ycsb.sh workloada 4 1000000 2`.