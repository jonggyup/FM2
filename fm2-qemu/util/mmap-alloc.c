/*
 * Support for RAM backed by mmaped host memory.
 *
 * Copyright (c) 2015 Red Hat, Inc.
 *
 * Authors:
 *  Michael S. Tsirkin <mst@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#ifdef CONFIG_LINUX
#include <linux/mman.h>
#else  /* !CONFIG_LINUX */
#define MAP_SYNC              0x0
#define MAP_SHARED_VALIDATE   0x0
#endif /* CONFIG_LINUX */

#include "qemu/osdep.h"
#include "qemu/mmap-alloc.h"
#include "qemu/host-utils.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include <sys/stat.h>

#define HUGETLBFS_MAGIC       0x958458f6

#ifdef CONFIG_LINUX
#include <sys/vfs.h>
#include <linux/magic.h>
#include <numaif.h>
#endif
#define CL_SIZE 64UL

#include "qemu/timer.h"

#include "qemu/thread.h"
#include "qemu/osdep.h" /* For g_get_num_cpus, g_new, g_free */


/* Arguments for each worker thread */
typedef struct {
    uintptr_t start;
    uintptr_t end;
} InvalidateRangeArgs;

/* The function each worker thread will execute */
static void *invalidate_worker_fn(void *opaque)
{
    InvalidateRangeArgs *args = (InvalidateRangeArgs *)opaque;
    uintptr_t p = args->start;
    uintptr_t end = args->end;

    /* Each thread flushes its assigned chunk */
    for (; p < end; p += CL_SIZE) {
        asm volatile("clflushopt %0" : "+m" (*(volatile char *)p));
    }
    return NULL;
}

/* * A QEMU-compliant parallel version of cxl_invalidate_range.
 * This function BLOCKS the calling thread until all 64GB are flushed.
 */
static void cxl_invalidate_range(void *addr, size_t len)
{
    uintptr_t start = (uintptr_t)addr & ~(CL_SIZE - 1);
    uintptr_t end = (uintptr_t)addr + len;
    size_t total_len = end - start;

    if (total_len == 0) {
        return;
    }

    int n_cpus = 16;
    if (n_cpus <= 0) {
        n_cpus = 1;
    }

    /* Don't create more threads than we have work to do */
    size_t num_lines = total_len / CL_SIZE;
    if (num_lines < n_cpus) {
        n_cpus = (int)num_lines;
        if (n_cpus == 0) n_cpus = 1;
    }

    QemuThread *threads = g_new(QemuThread, n_cpus);
    InvalidateRangeArgs *args = g_new(InvalidateRangeArgs, n_cpus);

    /* Calculate chunk size, ensuring it's cache-line-aligned */
    size_t chunk_size = (total_len + n_cpus - 1) / n_cpus;
    chunk_size = (chunk_size + CL_SIZE - 1) & ~(CL_SIZE - 1);

    uintptr_t chunk_start = start;
    int i;
    int n_threads_started = 0;

    for (i = 0; i < n_cpus; i++) {
        uintptr_t chunk_end = chunk_start + chunk_size;
        
        if (chunk_start >= end) {
            break; /* No work left */
        }
        if (chunk_end > end || i == n_cpus - 1) {
            chunk_end = end; /* Last thread takes the remainder */
        }

        args[i].start = chunk_start;
        args[i].end = chunk_end;

        qemu_thread_create(&threads[i], "cxl-invalidate-worker",
                           invalidate_worker_fn, &args[i],
                           QEMU_THREAD_JOINABLE);
        
        n_threads_started++;
        chunk_start = chunk_end;
    }

    /* Wait for all threads to finish */
    for (i = 0; i < n_threads_started; i++) {
        qemu_thread_join(&threads[i]);
    }

    g_free(threads);
    g_free(args);

    /* One final fence after all threads are done */
    asm volatile("sfence" ::: "memory");
}
/*
static inline void cxl_invalidate_range(void *addr, size_t len)
{
    uintptr_t p   = ((uintptr_t)addr) & ~(CL_SIZE - 1);
    uintptr_t end = (uintptr_t)addr + len;

    for (; p < end; p += CL_SIZE) {
        asm volatile("clflushopt %0" : "+m" (*(volatile char *)p));
    }
    asm volatile("sfence" ::: "memory");  // ensure all invalidations complete
}
*/
QemuFsType qemu_fd_getfs(int fd)
{
#ifdef CONFIG_LINUX
    struct statfs fs;
    int ret;

    if (fd < 0) {
        return QEMU_FS_TYPE_UNKNOWN;
    }

    do {
        ret = fstatfs(fd, &fs);
    } while (ret != 0 && errno == EINTR);

    switch (fs.f_type) {
    case TMPFS_MAGIC:
        return QEMU_FS_TYPE_TMPFS;
    case HUGETLBFS_MAGIC:
        return QEMU_FS_TYPE_HUGETLBFS;
    default:
        return QEMU_FS_TYPE_UNKNOWN;
    }
#else
    return QEMU_FS_TYPE_UNKNOWN;
#endif
}

size_t qemu_fd_getpagesize(int fd)
{
#ifdef CONFIG_LINUX
    struct statfs fs;
    int ret;

    if (fd != -1) {
        do {
            ret = fstatfs(fd, &fs);
        } while (ret != 0 && errno == EINTR);

        if (ret == 0 && fs.f_type == HUGETLBFS_MAGIC) {
            return fs.f_bsize;
        }
    }
#ifdef __sparc__
    /* SPARC Linux needs greater alignment than the pagesize */
    return QEMU_VMALLOC_ALIGN;
#endif
#endif

    return qemu_real_host_page_size();
}

#define OVERCOMMIT_MEMORY_PATH "/proc/sys/vm/overcommit_memory"
static bool map_noreserve_effective(int fd, uint32_t qemu_map_flags)
{
#if defined(__linux__)
    const bool readonly = qemu_map_flags & QEMU_MAP_READONLY;
    const bool shared = qemu_map_flags & QEMU_MAP_SHARED;
    gchar *content = NULL;
    const char *endptr;
    unsigned int tmp;

    /*
     * hugeltb accounting is different than ordinary swap reservation:
     * a) Hugetlb pages from the pool are reserved for both private and
     *    shared mappings. For shared mappings, all mappers have to specify
     *    MAP_NORESERVE.
     * b) MAP_NORESERVE is not affected by /proc/sys/vm/overcommit_memory.
     */
    if (qemu_fd_getpagesize(fd) != qemu_real_host_page_size()) {
        return true;
    }

    /*
     * Accountable mappings in the kernel that can be affected by MAP_NORESEVE
     * are private writable mappings (see mm/mmap.c:accountable_mapping() in
     * Linux). For all shared or readonly mappings, MAP_NORESERVE is always
     * implicitly active -- no reservation; this includes shmem. The only
     * exception is shared anonymous memory, it is accounted like private
     * anonymous memory.
     */
    if (readonly || (shared && fd >= 0)) {
        return true;
    }

    /*
     * MAP_NORESERVE is globally ignored for applicable !hugetlb mappings when
     * memory overcommit is set to "never". Sparse memory regions aren't really
     * possible in this system configuration.
     *
     * Bail out now instead of silently committing way more memory than
     * currently desired by the user.
     */
    if (g_file_get_contents(OVERCOMMIT_MEMORY_PATH, &content, NULL, NULL) &&
        !qemu_strtoui(content, &endptr, 0, &tmp) &&
        (!endptr || *endptr == '\n')) {
        if (tmp == 2) {
            error_report("Skipping reservation of swap space is not supported:"
                         " \"" OVERCOMMIT_MEMORY_PATH "\" is \"2\"");
            return false;
        }
        return true;
    }
    /* this interface has been around since Linux 2.6 */
    error_report("Skipping reservation of swap space is not supported:"
                 " Could not read: \"" OVERCOMMIT_MEMORY_PATH "\"");
    return false;
#endif
    /*
     * E.g., FreeBSD used to define MAP_NORESERVE, never implemented it,
     * and removed it a while ago.
     */
    error_report("Skipping reservation of swap space is not supported");
    return false;
}

/*
 * Reserve a new memory region of the requested size to be used for mapping
 * from the given fd (if any).
 */
static void *mmap_reserve(size_t size, int fd)
{
    int flags = MAP_PRIVATE;

#if defined(__powerpc64__) && defined(__linux__)
    /*
     * On ppc64 mappings in the same segment (aka slice) must share the same
     * page size. Since we will be re-allocating part of this segment
     * from the supplied fd, we should make sure to use the same page size, to
     * this end we mmap the supplied fd.  In this case, set MAP_NORESERVE to
     * avoid allocating backing store memory.
     * We do this unless we are using the system page size, in which case
     * anonymous memory is OK.
     */
    if (fd == -1 || qemu_fd_getpagesize(fd) == qemu_real_host_page_size()) {
        fd = -1;
        flags |= MAP_ANONYMOUS;
    } else {
        flags |= MAP_NORESERVE;
    }
#else
    fd = -1;
    flags |= MAP_ANONYMOUS;
#endif

    return mmap(0, size, PROT_NONE, flags, fd, 0);
}

int get_config_value(const char *key);
int get_config_value(const char *key) {
    FILE* file = fopen("./config.txt", "r");
    if (file == NULL) {
        puts("The file doesn't exist."); fflush(stdout);
        exit(-1);
    }
    char line[255] = {0}, value[255] = {0};
    char *endptr;
    int num = -1;

    while (fgets(line, 255, file)) {
        if (strncmp(line, key, strlen(key)) == 0) {
            strcpy(value, strchr(line, '=') + 1);
            value[strcspn(value, "\n")] = 0;

            num = strtol(value, &endptr, 10);
            break;
        }
    }
    fclose(file);
    return num;
}

/*
 * Activate memory in a reserved region from the given fd (if any),
 * to make it accessible.  Updated for kernel ≥ 6.8:
 *   • forces 2 MiB alignment/size,
 *   • MAP_POPULATE to reduce fragmentation,
 *   • MADV_HUGEPAGE hint for transparent‑hugepage promotion.
 */
static void *mmap_activate(void *ptr, size_t size, int fd,
                           uint32_t qemu_map_flags, off_t map_offset)
{
    const bool noreserve = qemu_map_flags & QEMU_MAP_NORESERVE;
    const bool readonly  = qemu_map_flags & QEMU_MAP_READONLY;
    const bool shared    = qemu_map_flags & QEMU_MAP_SHARED;
    const bool sync      = qemu_map_flags & QEMU_MAP_SYNC;

    int  prot  = PROT_READ | (readonly ? 0 : PROT_WRITE);
    int  flags = MAP_FIXED; //| MAP_POPULATE;        /* ensure early fault */
    int  map_sync_flags = 0;
    void *activated_ptr;

    /* Ensure size is a multiple of 2 MiB (required for THP) */
    #define HUGEPAGE_SZ (2UL * 1024 * 1024)
    size = (size + HUGEPAGE_SZ - 1) & ~(HUGEPAGE_SZ - 1);

    if (noreserve && !map_noreserve_effective(fd, qemu_map_flags)) {
        return MAP_FAILED;
    }

    flags |= (fd == -1 ? MAP_ANONYMOUS : 0);
    flags |= shared ? MAP_SHARED : MAP_PRIVATE;
    flags |= noreserve ? MAP_NORESERVE : 0;
    if (shared && sync) {
        map_sync_flags = MAP_SYNC | MAP_SHARED_VALIDATE;
    }

    /* ====== CONFIG: choose role (difference #1) ====== */
    const int cfg_is_src = get_config_value("IS_SOURCE_VM");
    const bool is_src = (cfg_is_src != 0);

    /*
     * One shm object distinguishes source vs. destination VM.
     * If open fails (not yet created) → source VM path.
     */
//    const char *dax_path              = get_config_value("CXL_DEV_PATH");

    /* ------------------------------------------------------------------ */
    /* SOURCE VM                                                          */
    /* ------------------------------------------------------------------ */
    if (is_src) {
        activated_ptr = mmap(ptr, size, prot, flags | map_sync_flags,
                             fd, map_offset);
        if (activated_ptr != MAP_FAILED) {
                // Prefault to avoid major-faults during the first big copy
            madvise(activated_ptr, size, MADV_WILLNEED);
            volatile char *c = (volatile char *)activated_ptr;
            for (size_t i = 0; i < size; i += 4096) c[i] = 0;
            madvise(activated_ptr, size, MADV_HUGEPAGE);        /* THP hint */

            int src_numa = get_config_value("SRC_NUMA");
            unsigned long nodemask = 1UL << src_numa;
            if (mbind(activated_ptr, size, MPOL_BIND,
                      &nodemask, 32, 0) != 0) {
                perror("mbind");
                exit(EXIT_FAILURE);
            }
            memset(activated_ptr, 0, size);   /* prefault */
        }
    }
    /* ------------------------------------------------------------------ */
    /* DESTINATION VM                                                     */
    /* ------------------------------------------------------------------ */
    else {
        int dst_numa              = get_config_value("DST_NUMA");
        int cxl_numa              = get_config_value("CXL_NUMA");
        int meta_state_length     = get_config_value("META_STATE_LENGTH");
        int hot_page_state_length = get_config_value("HOT_PAGE_STATE_LENGTH");

        assert(dst_numa != -1 && cxl_numa != -1 &&
               meta_state_length != -1 && hot_page_state_length != -1);

        if (size < 50 * 1024 * 1024) { /* small chunk */
            activated_ptr = mmap(ptr, size, prot,
                                 flags | map_sync_flags, fd, map_offset);
            if (activated_ptr != MAP_FAILED) {
                madvise(activated_ptr, size, MADV_HUGEPAGE);

                unsigned long nodemask = 1UL << dst_numa;
                if (mbind(activated_ptr, size, MPOL_BIND,
                          &nodemask, 32, 0) != 0) {
                    perror("mbind");
                    exit(EXIT_FAILURE);
                }
            memset(activated_ptr, 0, size);
            }
        } else {                         /* large chunk */
#define ALIGN_2MB (2UL * 1024 * 1024)
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
            int cxl_fd = open("/dev/dax1.0", O_RDWR);
            static uint64_t prefix_len = 0;
            printf("in large chunk dst mmap- begin\n");
            // 1. Calculate raw size needed
            uint64_t raw_size = 82946555904;
            
            // 2. Align the allocation size to 2MB (Good practice for Huge Pages)
            uint64_t cxl_size = ALIGN_UP(raw_size, ALIGN_2MB);

            void *cxl_ptr = mmap(0, cxl_size, PROT_READ|PROT_WRITE, MAP_SYNC|MAP_SHARED_VALIDATE, cxl_fd, 0);
//            void *cxl_ptr = mmap(0, cxl_size, PROT_READ|PROT_WRITE, MAP_SYNC|MAP_SHARED_VALIDATE, cxl_fd, 0);

            if (cxl_ptr == MAP_FAILED) {
                perror("mmap");
                exit(EXIT_FAILURE);
            }
            madvise(cxl_ptr, cxl_size, MADV_HUGEPAGE);

            activated_ptr = (char *)cxl_ptr
                            + meta_state_length
                            + hot_page_state_length
                            + prefix_len;
            prefix_len += cxl_size;
            printf("in large chunk dst mmap-end\n");
        }
    }

    /* ------------------------------------------------------------------ */
    /* Fallback if MAP_SYNC failed                                        */
    /* ------------------------------------------------------------------ */
    if (activated_ptr == MAP_FAILED && map_sync_flags) {
        activated_ptr = mmap(ptr, size, prot, flags, fd, map_offset);
        if (activated_ptr != MAP_FAILED) {
            madvise(activated_ptr, size, MADV_HUGEPAGE);
        }
            printf("mmap-fallback\n");
    }
    return activated_ptr;
}




static inline size_t mmap_guard_pagesize(int fd)
{
#if defined(__powerpc64__) && defined(__linux__)
    /* Mappings in the same segment must share the same page size */
    return qemu_fd_getpagesize(fd);
#else
    return qemu_real_host_page_size();
#endif
}

void *qemu_ram_mmap(int fd,
                    size_t size,
                    size_t align,
                    uint32_t qemu_map_flags,
                    off_t map_offset)
{
    const size_t guard_pagesize = mmap_guard_pagesize(fd);
    size_t offset, total;
    void *ptr, *guardptr;

    /*
     * Note: this always allocates at least one extra page of virtual address
     * space, even if size is already aligned.
     */
    total = size + align;

    guardptr = mmap_reserve(total, fd);
    if (guardptr == MAP_FAILED) {
        return MAP_FAILED;
    }

    assert(is_power_of_2(align));
    /* Always align to host page size */
    assert(align >= guard_pagesize);

    offset = QEMU_ALIGN_UP((uintptr_t)guardptr, align) - (uintptr_t)guardptr;

    ptr = mmap_activate(guardptr + offset, size, fd, qemu_map_flags,
                        map_offset);
    if (ptr == MAP_FAILED) {
        munmap(guardptr, total);
        return MAP_FAILED;
    }

    if (offset > 0) {
        munmap(guardptr, offset);
    }

    /*
     * Leave a single PROT_NONE page allocated after the RAM block, to serve as
     * a guard page guarding against potential buffer overflows.
     */
    total -= offset;
    if (total > size + guard_pagesize) {
        munmap(ptr + size + guard_pagesize, total - size - guard_pagesize);
    }

    return ptr;
}

void qemu_ram_munmap(int fd, void *ptr, size_t size)
{
    if (ptr) {
        /* Unmap both the RAM block and the guard page */
        munmap(ptr, size + mmap_guard_pagesize(fd));
    }
}
