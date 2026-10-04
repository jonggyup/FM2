//
//  ycsbc.cc
//  YCSB-C
//
//  Created by Jinglei Ren on 12/19/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#include <cstring>
#include <string>
#include <iostream>
#include <vector>
#include <future>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <atomic>

#include "core/utils.h"
#include "core/timer.h"
#include "core/client.h"
#include "core/core_workload.h"
#include "db/db_factory.h"

using namespace std;

void UsageMessage(const char *command);
bool StrStartWith(const char *str, const char *pre);
string ParseCommandLine(int argc, const char *argv[], utils::Properties &props);

#define WARMUP 10

std::vector<std::pair<std::pair<long long, long long>, int>> lat[100];

std::atomic<bool> atomicBool(false); // Initialize the atomic boolean variable

void notice_controller() {
  // asd123www_impl: tell controller the start of the transaction.
  FILE *pid_file = fopen("../../../controller.pid", "r");
  printf("File state: %d\n", pid_file == NULL);
  if (pid_file) {
    pid_t controller_pid;
    fscanf(pid_file, "%d", &controller_pid);
    fclose(pid_file);

    atomicBool.store(true);
    if (kill(controller_pid, SIGUSR1) == -1) {
      // perror("Error sending signal");
      // exit(-1);
    }
  }
}

int DelegateClient(ycsbc::DB *db, ycsbc::CoreWorkload *wl, const int num_ops, bool is_loading, int idx) {
  db->Init();
  ycsbc::Client client(*db, *wl);

  struct timespec start, end;
  lat[idx].clear();

  int oks = 0;
  for (int i = 0; i < num_ops; ++i) {
    if (is_loading) {
      oks += client.DoInsert();
    } else {
      if (atomicBool.load() == false) {
        oks += client.DoTransaction();
      } else {
        clock_gettime(CLOCK_MONOTONIC, &start);
        oks += client.DoTransaction();
        clock_gettime(CLOCK_MONOTONIC, &end);
        lat[idx].push_back(std::make_pair(make_pair(start.tv_sec * 1000000000LL + start.tv_nsec, 
                                                  end.tv_sec * 1000000000LL + end.tv_nsec - start.tv_sec * 1000000000LL - start.tv_nsec),
                                                  idx));
      }
    }
  }
  db->Close();
  return oks;
}

int main(const int argc, const char *argv[]) {
  utils::Properties props;
  string file_name = ParseCommandLine(argc, argv, props);

  const int num_threads = stoi(props.GetProperty("threadcount", "1"));

  std::vector<ycsbc::DB *> dbs;
  for (int i = 0; i < num_threads; i++) {
    ycsbc::DB *db = ycsbc::DBFactory::CreateDB(props);
    if (!db) {
      cout << "Unknown database name " << props["dbname"] << endl;
      exit(0);
    }
    dbs.push_back(db);
  }

  ycsbc::CoreWorkload wl;
  wl.Init(props);

  printf("# of threads: %d\n", num_threads);

  // Loads data
  vector<future<int>> actual_ops;
  int total_ops = stoi(props[ycsbc::CoreWorkload::RECORD_COUNT_PROPERTY]);
  for (int i = 0; i < num_threads; ++i) {
    actual_ops.emplace_back(async(launch::async,
        DelegateClient, dbs[i], &wl, total_ops / num_threads, true, i));
  }
  assert((int)actual_ops.size() == num_threads);

  int sum = 0;
  for (auto &n : actual_ops) {
    assert(n.valid());
    sum += n.get();
  }
  cerr << "# Loading records:\t" << sum << endl;

  // Peforms transactions
  actual_ops.clear();
  total_ops = stoi(props[ycsbc::CoreWorkload::OPERATION_COUNT_PROPERTY]);
  utils::Timer<double> timer;
  timer.Start();
  for (int i = 0; i < num_threads; ++i) {
    actual_ops.emplace_back(async(launch::async,
        DelegateClient, dbs[i], &wl, total_ops / num_threads, false, i));
  }
  assert((int)actual_ops.size() == num_threads);

  // warmpup for 10 seconds.
  puts("Warmup start.");fflush(stdout);
  sleep(WARMUP);
  puts("Warmup done.");fflush(stdout);
  notice_controller();

  sum = 0;
  for (auto &n : actual_ops) {
    assert(n.valid());
    sum += n.get();
  }

  double duration = timer.End();
  cerr << "# Transaction throughput (KTPS)" << endl;
  cerr << props["dbname"] << '\t' << file_name << '\t' << num_threads << '\t';
  cerr << total_ops / duration / 1000 << endl;

  for (int i = 0; i < num_threads; ++i) printf("size[%d]: %ld\n", i, lat[i].size());

  // write latency.
  std::ofstream file("redis_result.txt");
  for (int i = 1; i < num_threads; ++i) lat[0].insert(lat[0].end(), lat[i].begin(), lat[i].end());
  std::sort(lat[0].begin(), lat[0].end());
  for (uint32_t i = 0; i < lat[0].size(); ++i) file << lat[0][i].first.first << " " << lat[0][i].first.second << " " << lat[0][i].second <<std::endl;
}

string ParseCommandLine(int argc, const char *argv[], utils::Properties &props) {
  int argindex = 1;
  string filename;
  while (argindex < argc && StrStartWith(argv[argindex], "-")) {
    if (strcmp(argv[argindex], "-threads") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("threadcount", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-db") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("dbname", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-host") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("host", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-port") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("port", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-slaves") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("slaves", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-P") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      filename.assign(argv[argindex]);
      ifstream input(argv[argindex]);
      try {
        props.Load(input);
      } catch (const string &message) {
        cout << message << endl;
        exit(0);
      }
      input.close();
      argindex++;
    } else {
      cout << "Unknown option '" << argv[argindex] << "'" << endl;
      exit(0);
    }
  }

  if (argindex == 1 || argindex != argc) {
    UsageMessage(argv[0]);
    exit(0);
  }

  return filename;
}

void UsageMessage(const char *command) {
  cout << "Usage: " << command << " [options]" << endl;
  cout << "Options:" << endl;
  cout << "  -threads n: execute using n threads (default: 1)" << endl;
  cout << "  -db dbname: specify the name of the DB to use (default: basic)" << endl;
  cout << "  -P propertyfile: load properties from the given file. Multiple files can" << endl;
  cout << "                   be specified, and will be processed in the order specified" << endl;
}

inline bool StrStartWith(const char *str, const char *pre) {
  return strncmp(str, pre, strlen(pre)) == 0;
}