#define _GNU_SOURCE

#include <emmintrin.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <sched.h>

#define GB (1024UL * 1024UL * 1024UL)  // 1 GB in bytes
#define MB (1024UL * 1024UL)           // 1 MB in bytes

#define CXL_DEVICE_PATH "/dev/dax1.0"

#if defined(__GNUC__) || defined(__clang__)
#define MM_STREAM_SI64 _mm_stream_si64
#else
#error "Compiler not supported for this example."
#endif

void non_temporal_write_ul(unsigned long *dst, unsigned long value) {
    // Use _mm_stream_si64 for non-temporal store of 64-bit value
    MM_STREAM_SI64((long long int *)dst, (long long int)value);
}

static inline unsigned long long rdtsc() {
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

static inline uint64_t time_now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t microseconds = ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
    return microseconds;
}

uint64_t rand_u64() {
    uint64_t upper = (uint64_t)rand();
    uint64_t lower = (uint64_t)rand();
    return (upper << 32) | lower;
}

// Estimate the number of CPU cycles in one second
unsigned long long est_cycles_per_sec() {
    unsigned long long start = rdtsc();
    sleep(1);
    unsigned long long end = rdtsc();
    unsigned long long cycles_per_second = end - start;

    return cycles_per_second;
}

struct mem_access_args {
    char* memory;
    size_t size;
    size_t non_hot_freq;
    size_t interval_secs;
};

void* mem_access(void* _args) {
    struct mem_access_args* args = (struct mem_access_args *)_args;

    size_t hot_size_mb = 512;
    size_t hot_size = hot_size_mb * MB;  // Size of the hot subset
    size_t dram_size = 4 * GB;

    char* memory = args->memory;
    size_t size = args->size;
    size_t non_hot_freq = args->non_hot_freq;
    size_t interval_secs = args->interval_secs;

    // size_t current_hot_offset = 0;
    size_t current_hot_offset = dram_size;

    unsigned long long cycles_per_second = est_cycles_per_sec();
    // printf("cycles per second: %llu\n", cycles_per_second);
    printf("Changing hot set every %lu second(s) on average.\n", interval_secs);

    // Access different 1 GB subsets every second
    while (1) {
        size_t random_offset = rand_u64() % size;
        memory[random_offset] ^= 1;
    }

    return 0;
}

void *mmap_normal(size_t size) {
    // Allocate N GB of memory using mmap
    char *memory = (char *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }

    return memory;
}
void *mmap_cxl(size_t size) {
    // Open the device file
    int cxl_dax_dev = open(CXL_DEVICE_PATH, O_RDWR);
    if (cxl_dax_dev == -1) {
        perror("Error opening device file");
        exit(EXIT_FAILURE);
    }

    // Map the device memory into our address space
    void *cxl_mem =
        mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, cxl_dax_dev, 0);
    if (cxl_mem == MAP_FAILED) {
        perror("Error mapping device memory");
        close(cxl_dax_dev);
        exit(EXIT_FAILURE);
    }

    return cxl_mem;
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        printf("Expected 6 args, got %d args\n", argc);
        fprintf(stderr,
                "Usage: %s <N (number of GBs)> <K (every Kth access is "
                "non-hot)> <S (seconds before hotset change)> <NT (N threads)> [cxl/mmap]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    size_t N = (size_t)atoi(argv[1]);  // Number of GBs to allocate
    if (N == 0) {
        fprintf(stderr, "Error: N must be greater than 0.\n");
        return EXIT_FAILURE;
    }

    size_t K = (size_t)atoi(argv[2]);  // every k-th access is non-hot

    size_t S = (size_t)atoi(argv[3]);  // change hotset every S second.
    if (S == 0) {
        fprintf(stderr, "Error: S must be greater than 0.\n");
        return EXIT_FAILURE;
    }

    size_t NT = (size_t)atoi(argv[4]);
    if (NT == 0) {
        fprintf(stderr, "Error: NT must be greater than 0.\n");
        return EXIT_FAILURE;
    }

    int only_use_cxl = 0;
    if (argc == 6) {
        only_use_cxl = (int)atoi(argv[5]);
    }

    char *memory = 0;

    if (only_use_cxl) {
        memory = (char *)mmap_cxl(N * GB);
        printf("Allocated %zu GB of memory using mmap (CXL-only).\n", N);
    } else {
        memory = (char *)mmap_normal(N * GB);
        printf("Allocated %zu GB of memory using mmap.\n", N);
    }

    struct mem_access_args args;
    args.memory = memory;
    args.size = N * GB;
    args.non_hot_freq = K;
    args.interval_secs = S;

    size_t num_threads = NT;
    pthread_t threads[num_threads];
    printf("Spawning %lu threads...\n", num_threads);
    size_t first_cpu = 16;
    for (size_t i = 0; i < num_threads; i++) {
        int rc = pthread_create(&threads[i], NULL, mem_access, &args);
        assert(!rc);

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(first_cpu + i, &cpuset);
        rc = pthread_setaffinity_np(threads[i], sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            fprintf(stderr, "Error setting thread affinity: %s\n", strerror(rc));
            exit(-1);
        }
    }

    for (size_t i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    // mem_access(memory, N * GB, K, S);

    // Unmap the allocated memory (unreachable code, but good practice)
    if (munmap(memory, N * GB) == -1) {
        perror("munmap");
    }

    return EXIT_SUCCESS;
}