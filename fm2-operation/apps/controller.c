/* controller.c
 * Propagates thread-count env vars through sudo to VM boot scripts.
 * Expects boot scripts to pass env again when spawning QEMU (use: sudo env ... qemu-system-...).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // for bzero
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <assert.h>
#include <netdb.h>
#include <time.h>
#include <sys/types.h>
#include <signal.h>
#include <stdbool.h>

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#define MAX_LINE_LENGTH 255

#define TRIGGER_PATH "/sys/kernel/wbinvd_all/trigger"

static int write_sysfs(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
        return -1;
    }
    size_t len = strlen(val);
    ssize_t n = write(fd, val, len);
    int err = (n == (ssize_t)len) ? 0 : -1;
    if (err) fprintf(stderr, "write(%s): %s\n", path, strerror(errno));
    close(fd);
    return err;
}

static char g_envpfx[512] = "";  // filled in main()

static const char *getenv_or(const char *k, const char *defv) {
    const char *v = getenv(k);
    return (v && *v) ? v : defv;
}

int execute_wrapper(char *command) {
    char commandname[2048];
    snprintf(commandname, sizeof(commandname), "%s", command);
    return system(commandname);
}

int execute_wrapper_process(char *command) {
    pid_t pid = vfork();
    if (pid == -1) {
        perror("fork");
        exit(-1);
    } else if (pid == 0) {
        char *argv[] = {"sh", (char*)"-c", (char *)command, NULL};
        execvp("sh", argv);
        puts("Spawned process quited."); fflush(stdout);
        exit(-1);
    } else {
        return 0;
    }
}

void get_config_value(const char *key, char *value) {
    FILE* file = fopen("./config.txt", "r");
    if (file == NULL) {
        puts("The file doesn't exist."); fflush(stdout);
        exit(-1);
    }
    char line[MAX_LINE_LENGTH];
    while (fgets(line, MAX_LINE_LENGTH, file)) {
        if (strncmp(line, key, strlen(key)) == 0) {
            strcpy(value, strchr(line, '=') + 1);
            value[strcspn(value, "\n")] = 0;  // strip newline
            break;
        }
    }
    fclose(file);
}

int listen_wrapper(char *addr, char *port) {
    int sockfd, connfd, len;
    struct sockaddr_in servaddr, cli;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) { printf("socket creation failed...\n"); exit(0); }
    bzero(&servaddr, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(addr);
    servaddr.sin_port = htons(atoi(port));

    if ((bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr))) != 0) {
        perror("socket bind failed...\n");
        exit(1);
    }
    if ((listen(sockfd, 15)) != 0) { printf("Listen failed...\n"); exit(0); }

    len = sizeof(cli);
    connfd = accept(sockfd, (struct sockaddr *)&cli, (socklen_t *)&len);
    if (connfd < 0) { printf("server accept failed...\n"); exit(0); }
    return connfd;
}

int connect_wrapper(char *addr, char *port) {
    int sockfd;
    struct sockaddr_in server_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) { printf("socket creation failed...\n"); exit(0); }

    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(port));
    server_addr.sin_addr.s_addr = inet_addr(addr);

    int ret = connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0) { printf("connect failed!\n"); exit(0); }
    return sockfd;
}

/* ------------------------------ QEMU migration control ------------------------------ */

#define IP_LEN 64
#define PORT_LEN 20
#define DATA_LEN 1024

int connfd, srcfd, dstfd;

char src_ip[IP_LEN], dst_ip[IP_LEN], backup_ip[IP_LEN], vm_ip[IP_LEN];
char migration_port[PORT_LEN], src_control_port[PORT_LEN], dst_control_port[PORT_LEN], backup_control_port[PORT_LEN];
char buff[DATA_LEN];
struct timespec start, end;
char max_bandwidth[256], bench_script[256], cpu_num[256], memory_size[256], output_file[256], duration[256];
char write_through_duration[256];

char startString[15] = "start migration";
char endString[15]   = "ended migration";

void write_to_file(int fd, char *data) {
    uint32_t write_len = write(fd, data, strlen(data));
    assert(write_len == strlen(data));
}
void read_from_file(int fd, uint32_t len, char *data) {
    memset(data, 0, len + 1);
    uint32_t read_len = 0;
    while (1) {
        read_len = read(fd, data, len - 1);
        if (read_len) break;
    }
    assert(read_len == len - 1);
}

void signal_handler_src(int signal) {
    if (signal == SIGUSR1) {
        printf("src: Received SIGUSR1 signal\n"); fflush(stdout);
        write_to_file(connfd, "qemu_pre_copy_finish");
    }
}
// flag: 1 pre-copy, 0 post-copy.
void qemu_src_main(bool flag) {
    printf("Hello from the source!\n");
    FILE *pid_file = fopen("./src_controller.pid", "w");
    if (pid_file) { fprintf(pid_file, "%d", getpid()); fclose(pid_file); }

    if (signal(SIGUSR1, signal_handler_src) == SIG_ERR) {
        printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1);
    }

    char instr[2048];
    // inject env through sudo to the boot script
    snprintf(instr, sizeof(instr),
             "sudo env %s bash scripts/src_boot.sh %s %s %s > %s",
             g_envpfx, bench_script, cpu_num, memory_size, output_file);
    execute_wrapper_process(instr);

    snprintf(instr, sizeof(instr),
             "echo \"migrate_set_parameter max-bandwidth %s\" | sudo socat stdio unix-connect:qemu-monitor-migration-src",
             max_bandwidth);
    while (1) { int ret = execute_wrapper(instr); if (!ret) break; }
    printf("The target downtime is 150ms.\n");
    execute_wrapper("echo \"migrate_set_parameter downtime-limit 150\" | sudo socat stdio unix-connect:qemu-monitor-migration-src");
    if (!flag) {
        snprintf(instr, sizeof(instr),
                 "echo \"migrate_set_capability postcopy-ram on\" | sudo socat stdio unix-connect:qemu-monitor-migration-src");
        assert(execute_wrapper(instr) == 0);

        snprintf(instr, sizeof(instr),
                 "echo \"migrate_set_capability postcopy-preempt on\" | sudo socat stdio unix-connect:qemu-monitor-migration-src");
        assert(execute_wrapper(instr) == 0);
    }

    printf("addr:  %s:%s\n", src_ip, src_control_port);
    connfd = listen_wrapper(src_ip, src_control_port);

    read_from_file(connfd, sizeof("qemu_migrate"), buff);
    assert(strcmp(buff, "qemu_migrate") == 0);

    snprintf(instr, sizeof(instr),
             "echo \"migrate -d tcp:%s:%s\" | sudo socat stdio unix-connect:qemu-monitor-migration-src",
             dst_ip, migration_port);
    assert(execute_wrapper(instr) == 0);

    if (!flag) {
        snprintf(instr, sizeof(instr),
                 "echo \"migrate_start_postcopy\" | sudo socat stdio unix-connect:qemu-monitor-migration-src");
        assert(execute_wrapper(instr) == 0);
    }

    usleep(1000);
    execute_wrapper("sudo bash scripts/pin_vm_to_cores.sh src");

    while (1) { fflush(stdout); sleep(1); }
}
void rdma_src_main() {
    printf("Hello from the source!\n");
    FILE *pid_file = fopen("./src_controller.pid", "w");
    if (pid_file) { fprintf(pid_file, "%d", getpid()); fclose(pid_file); }

    if (signal(SIGUSR1, signal_handler_src) == SIG_ERR) {
        printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1);
    }

    char instr[2048];
    // inject env through sudo to the boot script
    snprintf(instr, sizeof(instr),
             "sudo env %s bash scripts/src_boot.sh %s %s %s > %s",
             g_envpfx, bench_script, cpu_num, memory_size, output_file);
    execute_wrapper_process(instr);

    snprintf(instr, sizeof(instr),
             "echo \"migrate_set_parameter max-bandwidth %s\" | sudo socat stdio unix-connect:qemu-monitor-migration-src",
             max_bandwidth);
    while (1) { int ret = execute_wrapper(instr); if (!ret) break; }


    printf("The target downtime is 150ms.\n");
    execute_wrapper("echo \"migrate_set_parameter downtime-limit 150\" | sudo socat stdio unix-connect:qemu-monitor-migration-src");


    printf("addr:  %s:%s\n", src_ip, src_control_port);
    connfd = listen_wrapper(src_ip, src_control_port);

    read_from_file(connfd, sizeof("qemu_migrate"), buff);
    assert(strcmp(buff, "qemu_migrate") == 0);

    snprintf(instr, sizeof(instr),
             "echo \"migrate -d rdma:%s:%s\" | sudo socat stdio unix-connect:qemu-monitor-migration-src",
             dst_ip, migration_port);
    assert(execute_wrapper(instr) == 0);

    usleep(1000);
    execute_wrapper("sudo bash scripts/pin_vm_to_cores.sh src");

    while (1) { fflush(stdout); sleep(1); }
}


volatile sig_atomic_t sigusr1_count = 0;
void signal_handler_dst_1(int signal) {
    if (signal == SIGUSR1) {
        printf("dst: Received SIGUSR1 signal\n"); fflush(stdout);
        write_to_file(connfd, !sigusr1_count?"qemu_vm_restart":"qemu_post_copy_finish");
        if (!sigusr1_count) {
            usleep(1000);
            execute_wrapper("sudo bash scripts/pin_vm_to_cores.sh dst");
        }
        sigusr1_count = 1;
    }
}
void signal_handler_dst_2(int signal) {
    if (signal == SIGUSR2) {
        printf("dst: Received SIGUSR2 signal\n"); fflush(stdout);
        write_to_file(connfd, !sigusr1_count?"qemu_vm_restart":"qemu_post_copy_finish");
        if (!sigusr1_count) {
            usleep(1000);
            execute_wrapper("sudo bash scripts/pin_vm_to_cores.sh dst");
        }
        sigusr1_count = 1;
    }
}
void qemu_dst_main(bool flag) {
    printf("Hello from the dest!\n");
    FILE *pid_file = fopen("./dst_controller.pid", "w");
    if (pid_file) { fprintf(pid_file, "%d", getpid()); fclose(pid_file); }

    if (signal(SIGUSR1, signal_handler_dst_1) == SIG_ERR) { printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1); }
    if (signal(SIGUSR2, signal_handler_dst_2) == SIG_ERR) { printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1); }

    char instr[2048];
    // inject env through sudo to the boot script
    snprintf(instr, sizeof(instr),
             "sudo env %s bash scripts/dst_boot.sh %s %s > %s",
             g_envpfx, cpu_num, memory_size, output_file);
    puts("Starting dest VM!"); fflush(stdout);
    printf("%s\n", instr);
    execute_wrapper_process(instr);

    snprintf(instr, sizeof(instr),
             "echo \"migrate_set_parameter max-bandwidth %s\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst",
             max_bandwidth);
    while (1) { int ret = execute_wrapper(instr); if (!ret) break; }

    if (!flag) {
        snprintf(instr, sizeof(instr),
                 "echo \"migrate_set_capability postcopy-ram on\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst");
        assert(execute_wrapper(instr) == 0);

        snprintf(instr, sizeof(instr),
                 "echo \"migrate_set_capability postcopy-preempt on\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst");
        assert(execute_wrapper(instr) == 0);
    }

    snprintf(instr, sizeof(instr),
             "echo \"migrate_incoming tcp:%s:%s\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst",
             dst_ip, migration_port);
    assert(execute_wrapper(instr) == 0);

    printf("addr:  %s:%s\n", dst_ip, dst_control_port);
    connfd = listen_wrapper(dst_ip, dst_control_port);

    while (1) { fflush(stdout); sleep(1); }
}

void rdma_dst_main() {
    printf("Hello from the dest!\n");
    FILE *pid_file = fopen("./dst_controller.pid", "w");
    if (pid_file) { fprintf(pid_file, "%d", getpid()); fclose(pid_file); }

    if (signal(SIGUSR1, signal_handler_dst_1) == SIG_ERR) { printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1); }
    if (signal(SIGUSR2, signal_handler_dst_2) == SIG_ERR) { printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1); }

    char instr[2048];
    // inject env through sudo to the boot script
    snprintf(instr, sizeof(instr),
             "sudo env %s bash scripts/dst_boot.sh %s %s > %s",
             g_envpfx, cpu_num, memory_size, output_file);
    puts("Starting dest VM!"); fflush(stdout);
    printf("%s\n", instr);
    execute_wrapper_process(instr);

    snprintf(instr, sizeof(instr),
             "echo \"migrate_set_parameter max-bandwidth %s\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst",
             max_bandwidth);
    while (1) { int ret = execute_wrapper(instr); if (!ret) break; }


    snprintf(instr, sizeof(instr),
             "echo \"migrate_incoming rdma:%s:%s\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst",
             dst_ip, migration_port);
    assert(execute_wrapper(instr) == 0);

    printf("addr:  %s:%s\n", dst_ip, dst_control_port);
    connfd = listen_wrapper(dst_ip, dst_control_port);

    while (1) { fflush(stdout); sleep(1); }
}


/*
void qemu_dst_main(bool flag) {
    printf("Hello from the dest!\n");
    FILE *pid_file = fopen("./dst_controller.pid", "w");
    if (pid_file) { fprintf(pid_file, "%d", getpid()); fclose(pid_file); }

    if (signal(SIGUSR1, signal_handler_dst_1) == SIG_ERR) { printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1); }
    if (signal(SIGUSR2, signal_handler_dst_2) == SIG_ERR) { printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1); }

    char instr[2048];
    // inject env through sudo to the boot script
    snprintf(instr, sizeof(instr),
             "sudo env %s bash scripts/dst_boot.sh %s %s > %s",
             g_envpfx, cpu_num, memory_size, output_file);
    puts("Starting dest VM!"); fflush(stdout);
    printf("%s\n", instr);
    execute_wrapper_process(instr);

    // 1. Set Max Bandwidth
    snprintf(instr, sizeof(instr),
             "echo \"migrate_set_parameter max-bandwidth %s\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst",
             max_bandwidth);
    while (1) { int ret = execute_wrapper(instr); if (!ret) break; }

    // --- NEW: Enable Multi-Threaded Migration (MultiFD) on Destination ---
    // Must be done BEFORE 'migrate_incoming'
    snprintf(instr, sizeof(instr),
             "echo \"migrate_set_capability multifd on\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst");
    assert(execute_wrapper(instr) == 0);

    snprintf(instr, sizeof(instr),
             "echo \"migrate_set_parameter multifd-channels 4\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst");
    assert(execute_wrapper(instr) == 0);
    // ---------------------------------------------------------------------

    if (!flag) {
        snprintf(instr, sizeof(instr),
                 "echo \"migrate_set_capability postcopy-ram on\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst");
        assert(execute_wrapper(instr) == 0);

        snprintf(instr, sizeof(instr),
                 "echo \"migrate_set_capability postcopy-preempt on\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst");
        assert(execute_wrapper(instr) == 0);
    }

    // 2. Start Listening for Incoming Migration
    snprintf(instr, sizeof(instr),
             "echo \"migrate_incoming tcp:%s:%s\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst",
             dst_ip, migration_port);
    assert(execute_wrapper(instr) == 0);

    printf("addr:  %s:%s\n", dst_ip, dst_control_port);
    connfd = listen_wrapper(dst_ip, dst_control_port);

    while (1) { fflush(stdout); sleep(1); }
}
*/
void signal_handler_backup(int signal) {
    if (signal == SIGUSR1) {
        printf("backup: Received SIGUSR1 signal\n"); fflush(stdout);

        printf("The wait to migrate duration is %d\n", atoi(write_through_duration));
        sleep(atoi(write_through_duration));

        struct timespec before_migrate, pre_copy_finish, vm_restart, post_copy_finish;
        clock_gettime(CLOCK_MONOTONIC, &before_migrate);
        write_to_file(srcfd, "qemu_migrate");

        read_from_file(srcfd, sizeof("qemu_pre_copy_finish"), buff);
        assert(strcmp(buff, "qemu_pre_copy_finish") == 0);
        clock_gettime(CLOCK_MONOTONIC, &pre_copy_finish);
        printf("pre-copy duration: %lld ns\n", pre_copy_finish.tv_sec * 1000000000LL + pre_copy_finish.tv_nsec - before_migrate.tv_sec * 1000000000LL - before_migrate.tv_nsec);
        fflush(stdout);

        read_from_file(dstfd, sizeof("qemu_vm_restart"), buff);
        printf("asd123www: %s\n", buff);
        assert(strcmp(buff, "qemu_vm_restart") == 0);
        clock_gettime(CLOCK_MONOTONIC, &vm_restart);

        read_from_file(dstfd, sizeof("qemu_post_copy_finish"), buff);
        assert(strcmp(buff, "qemu_post_copy_finish") == 0);
        clock_gettime(CLOCK_MONOTONIC, &post_copy_finish);

        printf("vm downtime: %lld ns\n", vm_restart.tv_sec * 1000000000LL + vm_restart.tv_nsec - pre_copy_finish.tv_sec * 1000000000LL - pre_copy_finish.tv_nsec);
        printf("post-copy duration: %lld ns\n", post_copy_finish.tv_sec * 1000000000LL + post_copy_finish.tv_nsec - vm_restart.tv_sec * 1000000000LL - vm_restart.tv_nsec);

        printf("Migration start: %lld ns\n", before_migrate.tv_sec * 1000000000LL + before_migrate.tv_nsec);
        printf("Migration end: %lld ns\n", post_copy_finish.tv_sec * 1000000000LL + post_copy_finish.tv_nsec);
        fflush(stdout);
    }
}
void qemu_backup_main() {
    printf("Hello from the backup!\n");
    printf("src:  %s:%s\n", src_ip, src_control_port);
    printf("dst:  %s:%s\n", dst_ip, dst_control_port); fflush(stdout);
    srcfd = connect_wrapper(src_ip, src_control_port);
    dstfd = connect_wrapper(dst_ip, dst_control_port);

    FILE *pid_file = fopen("./controller.pid", "w");
    if (pid_file) { fprintf(pid_file, "%d", getpid()); fclose(pid_file); }

    if (signal(SIGUSR1, signal_handler_backup) == SIG_ERR) {
        printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1);
    }

    while(1) { fflush(stdout); sleep(1); }
}

/* ------------------------------ SHM migration control ------------------------------ */

void signal_handler_shm_src_complete(int signal) {
    if (signal == SIGUSR1) {
        assert(sigusr1_count);
        printf("shm_src: VM image is complete!\n");
        fflush(stdout);
        write_to_file(connfd, "complete_vm_image");
        execute_wrapper("echo \"q\" | sudo socat stdio unix-connect:qemu-monitor-migration-src");
    }
}
void signal_handler_shm_src_precopy(int signal) {
    if (signal == SIGUSR2) {
        printf("shm_src: pre-copy has finished!\n");
        fflush(stdout);
        write_to_file(connfd, "shm_pre_copy_finish");
        sigusr1_count = 1;
    }
}
void shm_src_main() {
    printf("Hello from the shm_source!\n");
    FILE *pid_file = fopen("./src_controller.pid", "w");
    if (pid_file) { fprintf(pid_file, "%d", getpid()); fclose(pid_file); }

    if (signal(SIGUSR1, signal_handler_shm_src_complete) == SIG_ERR) { printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1); }
    if (signal(SIGUSR2, signal_handler_shm_src_precopy) == SIG_ERR) { printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1); }

    execute_wrapper("sudo rm /dev/shm/my_shared_memory");

    char instr[2048];
    // inject env through sudo to the boot script
    snprintf(instr, sizeof(instr),
             "sudo env %s bash scripts/src_boot.sh %s %s %s > %s",
             g_envpfx, bench_script, cpu_num, memory_size, output_file);
    execute_wrapper_process(instr);

    printf("addr:  %s:%s\n", src_ip, src_control_port);
    connfd = listen_wrapper(src_ip, src_control_port);

    read_from_file(connfd, sizeof("shm_migrate"), buff);
    assert(strcmp(buff, "shm_migrate") == 0);
    snprintf(instr, sizeof(instr),
             "echo \"migrate_FMSync_bandctrl %s\" | sudo socat stdio unix-connect:qemu-monitor-migration-src",
             getenv_or("FM_FMSYNC_BANDWIDTH_BPS", "0"));
    if (execute_wrapper(instr) != 0) {
        fprintf(stderr, "Unable to configure the FMSync bandwidth limit.\n");
        exit(EXIT_FAILURE);
    }
    snprintf(instr, sizeof(instr),
             "echo \"shm_migrate /my_shared_memory %d %s\" | sudo socat stdio unix-connect:qemu-monitor-migration-src",
             atoi(memory_size) + 1, duration);
    execute_wrapper(instr);

    read_from_file(connfd, sizeof("shm_migrate_switchover"), buff);
    assert(strcmp(buff, "shm_migrate_switchover") == 0);
    execute_wrapper("echo \"shm_migrate_switchover\" | sudo socat stdio unix-connect:qemu-monitor-migration-src");

    while(1) { fflush(stdout); sleep(1); }
}

void signal_handler_shm_dst(int signal) {
    if (signal == SIGUSR1) {
        write_to_file(connfd, "switchover_finished");
        clock_gettime(CLOCK_MONOTONIC, &end);
        printf("\n\ndst load VM image durtion: %lld ns\n",
               end.tv_sec * 1000000000LL + end.tv_nsec - start.tv_sec * 1000000000LL - start.tv_nsec);
        fflush(stdout);
    }
}
void shm_dst_main() {
    printf("Hello from the shm_destination!\n");
    FILE *pid_file = fopen("./dst_controller.pid", "w");
    if (pid_file) { fprintf(pid_file, "%d", getpid()); fclose(pid_file); }

    printf("addr:  %s:%s\n", dst_ip, dst_control_port);
    connfd = listen_wrapper(dst_ip, dst_control_port);

    if (signal(SIGUSR1, signal_handler_shm_dst) == SIG_ERR) {
        printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1);
    }
    char instr[2048];

    read_from_file(connfd, sizeof("start_target_vm"), buff);
    assert(strcmp(buff, "start_target_vm") == 0);

    // inject env through sudo to the boot script
    snprintf(instr, sizeof(instr),
             "sudo env %s bash scripts/dst_boot.sh %s %s > %s",
             g_envpfx, cpu_num, memory_size, output_file);
    puts("Starting dest VM!"); fflush(stdout);
    execute_wrapper_process(instr);
    snprintf(instr, sizeof(instr),
             "echo \"migrate_incoming_shm_setup /my_shared_memory %d\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst",
             atoi(memory_size) + 1);
    while (1) {
        int ret = execute_wrapper(instr);
        if (!ret) break;
    }



    read_from_file(connfd, sizeof("shm_load_vm_image"), buff);
    assert(strcmp(buff, "shm_load_vm_image") == 0);
    clock_gettime(CLOCK_MONOTONIC, &start);
    snprintf(instr, sizeof(instr),
             "echo \"migrate_incoming_shm /my_shared_memory %d\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst",
             atoi(memory_size) + 1);
    while (1) {
        int ret = execute_wrapper(instr);
        if (!ret) break;
    }
    sleep(1);
    snprintf(instr, sizeof(instr),
             "echo \"migrate_promotion_shm /my_shared_memory %d\" | sudo socat stdio unix-connect:qemu-monitor-migration-dst",
             atoi(memory_size) + 1);
    while (1) {
        int ret = execute_wrapper(instr);
        if (!ret) break;
    }
    while (1) { fflush(stdout); sleep(1); }

}

void signal_handler_shm_backup(int signal) {
        if (signal == SIGUSR1) {
        printf("backup: Received SIGUSR1 signal\n"); fflush(stdout);
        printf("The write-through duration is %d\n", atoi(write_through_duration));
        write_to_file(srcfd, "shm_migrate");
        sleep(atoi(write_through_duration)-5);
        write_to_file(dstfd, "start_target_vm");
        sleep(5);
        struct timespec before_migrate, pre_copy_finish, vm_restart;
        clock_gettime(CLOCK_MONOTONIC, &before_migrate);
        write_to_file(srcfd, "shm_migrate_switchover");
//        sleep(10);
        usleep(1000);

        read_from_file(srcfd, sizeof("shm_pre_copy_finish"), buff);
        assert(strcmp(buff, "shm_pre_copy_finish") == 0);
        clock_gettime(CLOCK_MONOTONIC, &pre_copy_finish);
        printf("pre-copy duration: %lld ns\n",
               pre_copy_finish.tv_sec * 1000000000LL + pre_copy_finish.tv_nsec
               - before_migrate.tv_sec * 1000000000LL - before_migrate.tv_nsec);
        fflush(stdout);

        usleep(300000);
        read_from_file(srcfd, sizeof("complete_vm_image"), buff);
        assert(strcmp(buff, "complete_vm_image") == 0);
        clock_gettime(CLOCK_MONOTONIC, &pre_copy_finish);
        write_to_file(dstfd, "shm_load_vm_image");
        read_from_file(dstfd, sizeof("switchover_finished"), buff);
        assert(strcmp(buff, "switchover_finished") == 0);
        clock_gettime(CLOCK_MONOTONIC, &vm_restart);

        printf("vm downtime: %lld ns\n",
               vm_restart.tv_sec * 1000000000LL + vm_restart.tv_nsec
               - pre_copy_finish.tv_sec * 1000000000LL - pre_copy_finish.tv_nsec);
        printf("Migration start: %lld ns\n", before_migrate.tv_sec * 1000000000LL + before_migrate.tv_nsec);
        printf("Migration end: %lld ns\n", vm_restart.tv_sec * 1000000000LL + vm_restart.tv_nsec);
        fflush(stdout);
    }
}

void shm_backup_main() {
    printf("Hello from the shm_backup!\n");
    printf("src:  %s:%s\n", src_ip, src_control_port);
    printf("dst:  %s:%s\n", dst_ip, dst_control_port); fflush(stdout);
    srcfd = connect_wrapper(src_ip, src_control_port);
    dstfd = connect_wrapper(dst_ip, dst_control_port);

    FILE *pid_file = fopen("./controller.pid", "w");
    if (pid_file) { fprintf(pid_file, "%d", getpid()); fclose(pid_file); }
    if (signal(SIGUSR1, signal_handler_shm_backup) == SIG_ERR) {
        printf("An error occurred while setting a signal handler.\n"); fflush(stdout); exit(-1);
    }

    while(1) { fflush(stdout); sleep(1); }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <migration mode: `shm`, `qemu-precopy`, or `qemu-postcopy`> <machine type: `src`, `dst`, or `backup`>\n", argv[0]);
        return 1;
    }
    printf("Migration mode: %s\n", argv[1]);
    printf("Machine type: %s\n", argv[2]);

    /* Build env prefix once; propagated via sudo env ... to boot scripts */
    snprintf(g_envpfx, sizeof(g_envpfx),
             "FM_MIG_COPY_THREADS=%s REC_HOT_NTHREADS=%s NTHREADS=%s REM_NTHREADS=%s CXL_SHM_PATH=%s CXLSHM_NODE=%s",
             getenv_or("FM_MIG_COPY_THREADS","1"),
             getenv_or("REC_HOT_NTHREADS","1"),
             getenv_or("NTHREADS", getenv_or("FM_MIG_COPY_THREADS","1")),
             getenv_or("REM_NTHREADS", getenv_or("REC_HOT_NTHREADS","1")),
             getenv_or("CXLSHM_PATH", "/dev/shm/my_shared_memory"),
             getenv_or("CXLSHM_NODE", "1"));
        
    fprintf(stderr, "[controller] envpfx: %s\n", g_envpfx);

    /* read the config file. */
    get_config_value("SRC_IP", src_ip);
    get_config_value("DST_IP", dst_ip);
    get_config_value("BACKUP_IP", backup_ip);
    get_config_value("VM_IP", vm_ip);
    get_config_value("MIGRATION_PORT", migration_port);
    get_config_value("SRC_CONTROL_PORT", src_control_port);
    get_config_value("DST_CONTROL_PORT", dst_control_port);
    get_config_value("BACKUP_CONTROL_PORT", backup_control_port);

    if (strcmp(argv[1], "shm") == 0) {
        if (strcmp(argv[2], "src") == 0) {
            if (argc < 8) {
                printf("Usage: %s shm src <bench_script> <#vCPU> <mem_size> <output_file> <duration_us>\n", argv[0]);
                return 1;
            }
            printf("benchmark script: %s\n", argv[3]);
            printf("# of vCPU: %s\n", argv[4]);
            printf("Memory size: %s\n", argv[5]);
            printf("Output file: %s\n", argv[6]);
            strcpy(bench_script, argv[3]);
            strcpy(cpu_num, argv[4]);
            strcpy(memory_size, argv[5]);
            strcpy(output_file, argv[6]);
            strcpy(duration, argv[7]);
            shm_src_main();
        } else if (strcmp(argv[2], "dst") == 0) {
            if (argc < 6) {
                printf("Usage: %s shm dst <#vCPU> <mem_size> <output_file>\n", argv[0]);
                return 1;
            }
            printf("# of vCPU: %s\n", argv[3]);
            printf("Memory size: %s\n", argv[4]);
            printf("Output file: %s\n", argv[5]);
            strcpy(cpu_num, argv[3]);
            strcpy(memory_size, argv[4]);
            strcpy(output_file, argv[5]);
            shm_dst_main();
        } else {
            if (argc < 4) {
                printf("Usage: %s shm backup <write_through_duration_sec>\n", argv[0]);
                return 1;
            }
            assert(strcmp(argv[2], "backup") == 0);
            strcpy(write_through_duration, argv[3]);
            shm_backup_main();
        }
    } else if (strcmp(argv[1], "rdma") == 0) {
        if (strcmp(argv[2], "src") == 0) {
            if (argc < 8) {
                printf("Usage: %s qemu- src <bench_script> <#vCPU> <mem_size> <output_file> <max-bandwidth>\n", argv[0]);
                return 1;
            }
            printf("benchmark script: %s\n", argv[3]);
            printf("# of vCPU: %s\n", argv[4]);
            printf("Memory size: %s\n", argv[5]);
            printf("Output file: %s\n", argv[6]);
            printf("Max-bandwidth: %s\n", argv[7]);
            strcpy(bench_script, argv[3]);
            strcpy(cpu_num, argv[4]);
            strcpy(memory_size, argv[5]);
            strcpy(output_file, argv[6]);
            strcpy(max_bandwidth, argv[7]);
            rdma_src_main();
        } else if (strcmp(argv[2], "dst") == 0) {
            if (argc < 7) {
                printf("Usage: %s qemu- dst <#vCPU> <mem_size> <output_file> <max-bandwidth>\n", argv[0]);
                return 1;
            }
            printf("# of vCPU: %s\n", argv[3]);
            printf("Memory size: %s\n", argv[4]);
            printf("Output file: %s\n", argv[5]);
            printf("Max-bandwidth: %s\n", argv[6]);
            strcpy(cpu_num, argv[3]);
            strcpy(memory_size, argv[4]);
            strcpy(output_file, argv[5]);
            strcpy(max_bandwidth, argv[6]);
            rdma_dst_main();
        } else {
            if (argc < 4) {
                printf("Usage: %s qemu- backup <wait_to_start_migrate_sec>\n", argv[0]);
                return 1;
            }
            assert(strcmp(argv[2], "backup") == 0);
            strcpy(write_through_duration, argv[3]);
            qemu_backup_main();
        }
    } else {
        bool flag = !strcmp(argv[1], "qemu-precopy");
        assert(flag || !strcmp(argv[1], "qemu-postcopy"));

        if (strcmp(argv[2], "src") == 0) {
            if (argc < 8) {
                printf("Usage: %s qemu- src <bench_script> <#vCPU> <mem_size> <output_file> <max-bandwidth>\n", argv[0]);
                return 1;
            }
            printf("benchmark script: %s\n", argv[3]);
            printf("# of vCPU: %s\n", argv[4]);
            printf("Memory size: %s\n", argv[5]);
            printf("Output file: %s\n", argv[6]);
            printf("Max-bandwidth: %s\n", argv[7]);
            strcpy(bench_script, argv[3]);
            strcpy(cpu_num, argv[4]);
            strcpy(memory_size, argv[5]);
            strcpy(output_file, argv[6]);
            strcpy(max_bandwidth, argv[7]);
            qemu_src_main(flag);
        } else if (strcmp(argv[2], "dst") == 0) {
            if (argc < 7) {
                printf("Usage: %s qemu- dst <#vCPU> <mem_size> <output_file> <max-bandwidth>\n", argv[0]);
                return 1;
            }
            printf("# of vCPU: %s\n", argv[3]);
            printf("Memory size: %s\n", argv[4]);
            printf("Output file: %s\n", argv[5]);
            printf("Max-bandwidth: %s\n", argv[6]);
            strcpy(cpu_num, argv[3]);
            strcpy(memory_size, argv[4]);
            strcpy(output_file, argv[5]);
            strcpy(max_bandwidth, argv[6]);
            qemu_dst_main(flag);
        } else {
            if (argc < 4) {
                printf("Usage: %s qemu- backup <wait_to_start_migrate_sec>\n", argv[0]);
                return 1;
            }
            assert(strcmp(argv[2], "backup") == 0);
            strcpy(write_through_duration, argv[3]);
            qemu_backup_main();
        }
    }

    return 0;
}

