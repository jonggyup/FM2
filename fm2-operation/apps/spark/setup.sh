# spark
cd ~/spark
# sudo apt update
# sudo apt install openjdk-8-jdk -y
# sudo apt install openjdk-8-jre-headless openjdk-8-jdk-headless -y
# sudo apt install maven -y
echo 'export JAVA_HOME=/usr/lib/jvm/java-8-openjdk-amd64' >> ~/.bashrc
source ~/.bashrc
bash -c "echo 'export JAVA_HOME=/usr/lib/jvm/java-8-openjdk-amd64' >> /etc/environment"
source /etc/environment
# echo 'export JAVA_HOME=/usr/lib/jvm/java-8-openjdk-amd64' >> ~/.bashrc
# source ~/.bashrc
# sudo bash -c "echo 'export JAVA_HOME=/usr/lib/jvm/java-8-openjdk-amd64' >> /etc/environment"
# source /etc/environment

cd ~/spark
wget https://archive.apache.org/dist/hadoop/common/hadoop-3.2.4/hadoop-3.2.4.tar.gz
tar -xzvf hadoop-3.2.4.tar.gz
mv hadoop-3.2.4 hadoop

cd hadoop
# sed -i "s|if \[\[ -e '\''/usr/bin/pdsh'\'' \]\]; then|if [[ ! -e '/usr/bin/pdsh' ]]; then|" libexec/hadoop-functions.sh
sed -i "s|<configuration>||" etc/hadoop/core-site.xml
sed -i "s|</configuration>||" etc/hadoop/core-site.xml
echo "<configuration>" >> etc/hadoop/core-site.xml
echo "<property>" >> etc/hadoop/core-site.xml
echo "<name>fs.defaultFS</name>" >> etc/hadoop/core-site.xml
echo "<value>hdfs://syzkaller:8020</value>" >> etc/hadoop/core-site.xml
echo "</property>" >> etc/hadoop/core-site.xml
echo "</configuration>" >> etc/hadoop/core-site.xml

sed -i "s|<configuration>||" etc/hadoop/hdfs-site.xml
sed -i "s|</configuration>||" etc/hadoop/hdfs-site.xml
cat >> etc/hadoop/hdfs-site.xml <<- End
<configuration>
    <property>
        <name>dfs.replication</name>
        <value>1</value>
    </property>
    <property>
        <name>dfs.datanode.data.dir</name>
        <value>~/hdfs/datanode</value>
    </property>
</configuration>
End

mkdir -p ~/hdfs/datanode
chmod -R 777 ~/hdfs/datanode
chmod -R 777 ~/spark/hadoop/logs

ssh-keygen -t rsa -P '' -f ~/.ssh/id_rsa
cat ~/.ssh/id_rsa.pub >> ~/.ssh/authorized_keys
cat ~/.ssh/id_rsa.pub >> ~/.ssh/authorized_keys
cat ~/.ssh/id_rsa.pub >> ~/.ssh/authorized_keys
chmod 0600 ~/.ssh/authorized_keys

# Formate the filesystem
bin/hdfs namenode -format

echo 'export PDSH_RCMD_TYPE=ssh' >> ~/.bashrc
source ~/.bashrc

sed -i "s|<configuration>||" etc/hadoop/mapred-site.xml
sed -i "s|</configuration>||" etc/hadoop/mapred-site.xml
HADOOP_MAPRED_HOME='$HADOOP_MAPRED_HOME'
echo $HADOOP_MAPRED_HOME
sleep 10
cat >> etc/hadoop/mapred-site.xml <<- End
<configuration>
    <property>
        <name>mapreduce.framework.name</name>
        <value>yarn</value>
    </property>
    <property>
        <name>yarn.app.mapreduce.am.env</name>
        <value>HADOOP_MAPRED_HOME=/root/spark/hadoop</value>
    </property>
    <property>
        <name>mapreduce.map.env</name>
        <value>HADOOP_MAPRED_HOME=/root/spark/hadoop</value>
    </property>
    <property>
        <name>mapreduce.reduce.env</name>
        <value>HADOOP_MAPRED_HOME=/root/spark/hadoop</value>
    </property>
    <property>
        <name>mapreduce.application.classpath</name>
        <value>$HADOOP_MAPRED_HOME/share/hadoop/mapreduce/*,$HADOOP_MAPRED_HOME/share/hadoop/mapreduce/lib/*,$HADOOP_MAPRED_HOME/share/hadoop/common/*,$HADOOP_MAPRED_HOME/share/hadoop/common/lib/*,$HADOOP_MAPRED_HOME/share/hadoop/yarn/*,$HADOOP_MAPRED_HOME/share/hadoop/yarn/lib/*,$HADOOP_MAPRED_HOME/share/hadoop/hdfs/*,$HADOOP_MAPRED_HOME/share/hadoop/hdfs/lib/*</value>
    </property>
</configuration>
End

sed -i "s|<configuration>||" etc/hadoop/yarn-site.xml
sed -i "s|</configuration>||" etc/hadoop/yarn-site.xml
cat >> etc/hadoop/yarn-site.xml <<- End
<configuration>
    <property>
        <name>yarn.nodemanager.aux-services</name>
        <value>mapreduce_shuffle</value>
    </property>
    <property>
        <name>yarn.nodemanager.env-whitelist</name>
        <value>JAVA_HOME,HADOOP_COMMON_HOME,HADOOP_HDFS_HOME,HADOOP_CONF_DIR,CLASSPATH_PREPEND_DISTCACHE,HADOOP_YARN_HOME,HADOOP_HOME,PATH,LANG,TZ,HADOOP_MAPRED_HOME</value>
    </property>
</configuration>
End

cd ~/spark
wget https://archive.apache.org/dist/spark/spark-2.4.0/spark-2.4.0-bin-hadoop2.7.tgz
tar -xzvf spark-2.4.0-bin-hadoop2.7.tgz
mv spark-2.4.0-bin-hadoop2.7 spark
cd spark
echo "export SPARK_HOME=$(pwd)" >> ~/.bashrc
echo 'export PATH=$PATH:$SPARK_HOME/bin' >> ~/.bashrc
source ~/.bashrc

# cd ~/spark
# wget https://mirrors.estointernet.in/apache/maven/maven-3/3.6.3/binaries/apache-maven-3.6.3-bin.tar.gz
# tar -xzvf apache-maven-3.6.3-bin.tar.gz
# mv apache-maven-3.6.3 apache-maven
# cd apache-maven
# echo "export M2_HOME=$(pwd)" >> ~/.bashrc
# echo 'export PATH=$PATH:$M2_HOME/bin' >> ~/.bashrc
# source ~/.bashrc

cd ~/spark
wget https://github.com/Intel-bigdata/HiBench/archive/refs/tags/v7.1.1.tar.gz
tar -xzvf v7.1.1.tar.gz
mv HiBench-7.1.1 HiBench
cd HiBench

cp hadoopbench/mahout/pom.xml hadoopbench/mahout/pom.xml.bak
cat hadoopbench/mahout/pom.xml \
    | sed 's|<repo2>http://archive.cloudera.com</repo2>|<repo2>https://archive.apache.org</repo2>|' \
    | sed 's|cdh5/cdh/5/mahout-0.9-cdh5.1.0.tar.gz|dist/mahout/0.9/mahout-distribution-0.9.tar.gz|' \
    | sed 's|aa953e0353ac104a22d314d15c88d78f|09b999fbee70c9853789ffbd8f28b8a3|' \
    > ./pom.xml.tmp
mv ./pom.xml.tmp hadoopbench/mahout/pom.xml

mvn -Phadoopbench -Psparkbench -Dspark=2.4 -Dscala=2.11 clean package
cp conf/hadoop.conf.template conf/hadoop.conf
sed -i "s|^hibench.hadoop.home.*|hibench.hadoop.home ~/spark/hadoop|" conf/hadoop.conf
echo "hibench.hadoop.examples.jar ~/spark/hadoop/share/hadoop/mapreduce/hadoop-mapreduce-examples-3.2.4.jar" >> conf/hadoop.conf
echo "hibench.hadoop.examples.test.jar ~/spark/hadoop/share/hadoop/mapreduce/hadoop-mapreduce-client-jobclient-3.2.4-tests.jar" >> conf/hadoop.conf

cp conf/spark.conf.template conf/spark.conf
sed -i "s|hibench.spark.home.*|hibench.spark.home ~/spark/spark|" conf/spark.conf
sed -i "s|hibench.yarn.executor.num.*|hibench.yarn.executor.num 2|" conf/spark.conf
sed -i "s|hibench.yarn.executor.cores.*|hibench.yarn.executor.cores 2|" conf/spark.conf
sed -i "s|spark.executor.memory.*|spark.executor.memory 2g|" conf/spark.conf
sed -i "s|spark.driver.memory.*|spark.driver.memory 2g|" conf/spark.conf

echo "hibench.masters.hostnames syzkaller" >> conf/spark.conf
echo "hibench.slaves.hostnames syzkaller" >> conf/spark.conf

sed -i "s|hibench.scale.profile.*|hibench.scale.profile large|" conf/hibench.conf