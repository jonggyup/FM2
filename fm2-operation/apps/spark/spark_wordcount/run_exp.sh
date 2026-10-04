source ~/.bashrc
cd ~/spark/HiBench
bin/workloads/micro/wordcount/spark/run.sh
cat ~/spark/HiBench/report/hibench.report
# cp ~/spark/HiBench/report/hibench.report /home/ubuntu/result_app_perf.txt