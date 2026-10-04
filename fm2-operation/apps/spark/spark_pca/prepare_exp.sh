cd /root/spark
rm -f result_*
cd /root/spark/HiBench
rm -f report/hibench.report

source ~/.bashrc
yes "Y" | ~/spark/hadoop/bin/hdfs namenode -format
rm -rf ~/hdfs/datanode/*

export HDFS_NAMENODE_USER="root"
export HDFS_DATANODE_USER="root"
export HDFS_SECONDARYNAMENODE_USER="root"
export YARN_RESOURCEMANAGER_USER="root"
export YARN_NODEMANAGER_USER="root"

~/spark/hadoop/sbin/start-dfs.sh
~/spark/hadoop/sbin/start-yarn.sh
~/spark/spark/sbin/start-master.sh

bin/workloads/ml/pca/prepare/prepare.sh
