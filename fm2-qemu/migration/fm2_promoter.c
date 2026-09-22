#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/userfaultfd.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/time.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <time.h>

#include "fm2_promoter.h"
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/error-report.h"
#include <stdatomic.h>

/* Jonggyu: this file is the QEMU-side FMLift implementation. Upstream QEMU
 * does not promote CXL-backed guest memory; FM2 uses userfaultfd to serialize
 * missing/write-protect faults while a 2 MiB region is copied to DRAM. */
int promote_cxl_to_dram(void *start_addr, size_t length);

#define METADATA_SIZE   (10 * 1024 * 1024)   /* 10MB */
#define PROMOTION_UNIT  (2 * 1024 * 1024)    /* 2MB */
#define BASE_PAGE       4096                 /* 4K base page */

#define ERR_UFFD_CREATE    -1
#define ERR_UFFD_REGISTER  -2
#define ERR_MEMORY_ALLOC   -3
#define ERR_MMAP_FAILED    -4
#define ERR_WP_FAILED      -5

/* Max number of chunks simultaneously "in flight" (write-protected) */
#define MAX_INFLIGHT   1

/* Jonggyu: these offsets mirror the FMSync metadata layout in ram.c. */
#define FM2_HOT_MAGIC            0x484F5431u
#define FM2_HOT_VERSION          1
#define FM2_HOT_ENTRY_BYTES      16
#define FM2_HOT_KEY_SHIFT        21

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t epoch;
    uint32_t cap;
    uint32_t entry_bytes;
    uint32_t n_items;
    uint32_t pad;
} Fm2HotHdr;

typedef struct {
    uint64_t key_off_2m;
    uint32_t last_epoch;
    uint16_t freq;
    uint16_t pad;
} Fm2HotEntry;

typedef struct {
    size_t index;
    uint32_t epoch;
    uint16_t freq;
    bool hot;
} Fm2PromotionOrder;

static _Atomic uint64_t g_fmlift_bandwidth_bps;

int fm2_promoter_set_bandwidth(uint64_t bytes_per_sec)
{
    atomic_store_explicit(&g_fmlift_bandwidth_bps, bytes_per_sec,
                          memory_order_relaxed);
    return 0;
}

/* Jonggyu: pace CXL-to-DRAM copies in 100 ms windows. A single 2 MiB
 * promotion is allowed even when the configured quota is smaller than one
 * chunk, preventing a low cap from turning into a permanent wait. */
static void fm2_promoter_throttle(size_t bytes)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t window_start_ns;
    static uint64_t window_bytes;
    const uint64_t window_ns = 100000000ULL;

    for (;;) {
        uint64_t cap = atomic_load_explicit(&g_fmlift_bandwidth_bps,
                                            memory_order_relaxed);
        if (!cap) {
            return;
        }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        uint64_t quota = cap / 10;
        if (!quota) {
            quota = 1;
        }

        pthread_mutex_lock(&mu);
        if (!window_start_ns || now_ns - window_start_ns >= window_ns) {
            window_start_ns = now_ns;
            window_bytes = 0;
        }

        if (!window_bytes || window_bytes + bytes <= quota) {
            window_bytes += bytes;
            pthread_mutex_unlock(&mu);
            return;
        }

        uint64_t remaining_ns = window_ns - (now_ns - window_start_ns);
        pthread_mutex_unlock(&mu);

        struct timespec delay = {
            .tv_sec = remaining_ns / 1000000000ULL,
            .tv_nsec = remaining_ns % 1000000000ULL,
        };
        nanosleep(&delay, NULL);
    }
}

typedef struct {
    uint64_t pages_promoted;
    uint64_t pages_skipped;
    uint64_t wp_faults_handled;
    uint64_t promotion_failures;
} PromoterStats;

static PromoterStats g_stats = {0};

typedef struct {
    void   *addr;    /* base of 2MB region */
    size_t  len;     /* PROMOTION_UNIT */
    bool    active;
    bool    done;
} InflightRegion;

typedef struct {
    void   *start_addr;        /* aligned VMA start */
    size_t  length;            /* total bytes */

    int     uffd;
    int     epoll_fd;
    bool    monitor_active;

    pthread_mutex_t inflight_mu;
    pthread_cond_t  inflight_cv;
    InflightRegion  inflight[MAX_INFLIGHT];

    /* Shadow mapping for the entire DevDAX VMA */
    void   *shadow_base;

    /* Range we actually promote (after skipping metadata & aligning) */
    void   *promotion_start;
    size_t  promotion_length;

    pthread_t monitor_thread;
} PagePromoter;

/* --------------------------------------------------------------------- */
/* Helper: detect whether an address is in a /dev/dax mapping (debug)    */
/* --------------------------------------------------------------------- */
static bool is_devdax_mapping(void *addr)
{
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        return false;
    }

    char line[512];
    bool is_dax = false;
    uintptr_t target = (uintptr_t)addr;

    while (fgets(line, sizeof(line), fp)) {
        uintptr_t start, end;
        char perms[5], offset[17], dev[12], inode[24], path[256] = {0};
        int n = sscanf(line, "%lx-%lx %4s %16s %11s %23s %255[^\n]",
                       &start, &end, perms, offset, dev, inode, path);
        if (n >= 6 && target >= start && target < end) {
            if (strstr(path, "/dev/dax")) {
                is_dax = true;
            }
            break;
        }
    }
    fclose(fp);
    return is_dax;
}

/* --------------------------------------------------------------------- */
/* userfaultfd setup / registration                                      */
/* --------------------------------------------------------------------- */
static int setup_userfaultfd(void)
{
    int uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (uffd == -1) {
        perror("userfaultfd");
        return -1;
    }

    struct uffdio_api api = {
        .api      = UFFD_API,
        .features = UFFD_FEATURE_PAGEFAULT_FLAG_WP,
    };
    if (ioctl(uffd, UFFDIO_API, &api) == -1) {
        perror("ioctl-UFFDIO_API");
        close(uffd);
        return -1;
    }

    if (!(api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP)) {
        fprintf(stderr, "Kernel does not support UFFD_FEATURE_PAGEFAULT_FLAG_WP\n");
        close(uffd);
        return -1;
    }

    return uffd;
}

/* Always register with MISSING + WP (DevDAX path) */
static int register_uffd_range(int uffd, void *addr, size_t len)
{
    struct uffdio_register reg = {0};

    reg.range.start = (unsigned long)addr;
    reg.range.len   = len;
    reg.mode        = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;

    if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1) {
        perror("ioctl-UFFDIO_REGISTER");
        return -1;
    }

    if (!(reg.ioctls & (1ULL << _UFFDIO_WRITEPROTECT))) {
        fprintf(stderr, "UFFDIO_WRITEPROTECT not available after registration\n");
        return -1;
    }

    return 0;
}

static int unregister_uffd_range(int uffd, void *addr, size_t len)
{
    struct uffdio_range r = {
        .start = (unsigned long)addr,
        .len   = len,
    };
    if (ioctl(uffd, UFFDIO_UNREGISTER, &r) == -1 && errno != ENOENT) {
        perror("ioctl-UFFDIO_UNREGISTER");
        return -1;
    }
    return 0;
}

static int wp_protect_range(int uffd, void *addr, size_t len)
{
    struct uffdio_writeprotect wp = {
        .range = {
            .start = (unsigned long)addr,
            .len   = len,
        },
        .mode  = UFFDIO_WRITEPROTECT_MODE_WP,
    };

    if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
        perror("ioctl-UFFDIO_WRITEPROTECT (protect)");
        return -1;
    }
    return 0;
}

static int wp_unprotect_range(int uffd, void *addr, size_t len)
{
    struct uffdio_writeprotect wp = {
        .range = {
            .start = (unsigned long)addr,
            .len   = len,
        },
        .mode  = 0,
    };

    if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
        if (errno == ENOENT) {
            /* Already unprotected or unregistered */
            return 0;
        }
        perror("ioctl-UFFDIO_WRITEPROTECT (unprotect)");
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------- */
/* Shadow mapping for DevDAX                                             */
/* --------------------------------------------------------------------- */
/*
 * Map a read-only shadow region that covers [region_addr, region_addr+len)
 * from the same /dev/dax file and offset as the original mapping.
 */
static void *map_shadow_dax_region(void *region_addr, size_t len)
{
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        perror("fopen(/proc/self/maps)");
        return MAP_FAILED;
    }

    uintptr_t target = (uintptr_t)region_addr;
    char line[512];

    uintptr_t vma_start = 0;
    unsigned long file_off_start = 0;
    char path[256] = {0};
    bool found = false;

    while (fgets(line, sizeof(line), fp)) {
        uintptr_t start, end;
        char perms[5], off_str[17], dev[12], inode[24], p[256] = {0};

        int n = sscanf(line, "%lx-%lx %4s %16s %11s %23s %255[^\n]",
                       &start, &end, perms, off_str, dev, inode, p);
        if (n < 6) {
            continue;
        }

        if (!strstr(p, "/dev/dax")) {
            continue;
        }

        if (target >= start && target < end) {
            vma_start = start;
            file_off_start = strtoul(off_str, NULL, 16);
            strncpy(path, p, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            found = true;
            break;
        }
    }

    fclose(fp);

    if (!found) {
        fprintf(stderr, "map_shadow_dax_region: could not find DevDAX VMA for %p\n",
                region_addr);
        return MAP_FAILED;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open(/dev/dax...) for shadow");
        return MAP_FAILED;
    }

    /* offset in the dax device */
    off_t off = (off_t)file_off_start +
                (off_t)((uintptr_t)region_addr - vma_start);

    void *shadow = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, off);
    if (shadow == MAP_FAILED) {
        perror("mmap shadow DevDAX");
    }

    close(fd);
    return shadow;
}

/* --------------------------------------------------------------------- */
/* Inflight tracking helpers                                             */
/* --------------------------------------------------------------------- */
static int inflight_add_wait(PagePromoter *pr, void *addr, size_t len, int *slot_out)
{
    pthread_mutex_lock(&pr->inflight_mu);

    for (;;) {
        for (int i = 0; i < MAX_INFLIGHT; ++i) {
            if (!pr->inflight[i].active) {
                pr->inflight[i].addr   = addr;
                pr->inflight[i].len    = len;
                pr->inflight[i].active = true;
                pr->inflight[i].done   = false;
                if (slot_out) {
                    *slot_out = i;
                }
                pthread_mutex_unlock(&pr->inflight_mu);
                return 0;
            }
        }

        /* Wait until some region finishes */
        pthread_cond_wait(&pr->inflight_cv, &pr->inflight_mu);
    }
}

static void inflight_mark_done_slot(PagePromoter *pr, int slot)
{
    pthread_mutex_lock(&pr->inflight_mu);
    pr->inflight[slot].done   = true;
    pr->inflight[slot].active = false;
    pthread_cond_broadcast(&pr->inflight_cv);
    pthread_mutex_unlock(&pr->inflight_mu);
}

/* Wait if fault_page lies inside any in-flight region that isn't done.
 * Returns true if the fault_page belongs to any in-flight region
 * (even if that region is already done by the time we return),
 * and false if it was never covered by any inflight entry.
 */
static bool wait_if_inflight(PagePromoter *pr, void *fault_page)
{
    uintptr_t f = (uintptr_t)fault_page;
    bool seen = false;

    pthread_mutex_lock(&pr->inflight_mu);

    for (;;) {
        bool should_wait = false;

        for (int i = 0; i < MAX_INFLIGHT; ++i) {
            if (!pr->inflight[i].active)
                continue;

            uintptr_t base = (uintptr_t)pr->inflight[i].addr;
            uintptr_t end  = base + pr->inflight[i].len;

            if (f >= base && f < end) {
                /* We found a covering inflight region. */
                seen = true;

                /* Only wait if it is not done yet. */
                if (!pr->inflight[i].done)
                    should_wait = true;

                break;
            }
        }

        if (!should_wait || !pr->monitor_active)
            break;

        /* Wait until some promotion finishes and signals inflight_cv. */
        pthread_cond_wait(&pr->inflight_cv, &pr->inflight_mu);
    }

    pthread_mutex_unlock(&pr->inflight_mu);
    return seen;
}
/* On-demand promotion triggered directly by a MISSING fault.
 * We copy from the shadow DevDAX mapping into a fresh anonymous 2MB chunk,
 * then mremap it into the faulting VA.
 */
static int promote_region_from_fault(PagePromoter *pr, void *fault_page)
{
    void *tmp;

    /* 1) Allocate a temporary anonymous 2MB destination */
    tmp = mmap(NULL, PROMOTION_UNIT,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
               -1, 0);
    if (tmp == MAP_FAILED) {
        perror("mmap anonymous tmp (fault path)");
        g_stats.promotion_failures++;
        return ERR_MMAP_FAILED;
    }

    /* 2) Copy from shadow DevDAX mapping into anon tmp */
    ptrdiff_t offset = (char *)fault_page - (char *)pr->start_addr;
    void *src        = (char *)pr->shadow_base + offset;
    memcpy(tmp, src, PROMOTION_UNIT);

    /* 3) Atomically replace DevDAX with anon pages at the original VA */
    void *moved = mremap(tmp, PROMOTION_UNIT, PROMOTION_UNIT,
                         MREMAP_MAYMOVE | MREMAP_FIXED,
                         fault_page);
    if (moved == MAP_FAILED) {
        int err = errno;
        perror("mremap to target region (fault path)");
        munmap(tmp, PROMOTION_UNIT);
        g_stats.promotion_failures++;
        return -err;
    }

    g_stats.pages_promoted++;
    return 0;
}


static void *fault_handler_thread(void *arg)
{
    PagePromoter *pr = (PagePromoter *)arg;
    struct epoll_event evs[8];

    while (pr->monitor_active) {
        int n = epoll_wait(pr->epoll_fd, evs, 8, 1000); /* 1s timeout */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }
        if (n == 0)
            continue;

        for (int i = 0; i < n; ++i) {
            if (!(evs[i].events & EPOLLIN))
                continue;

            for (;;) {
                struct uffd_msg msg;
                ssize_t r = read(pr->uffd, &msg, sizeof(msg));
                if (r == -1 && errno == EAGAIN)
                    break;
                if (r <= 0)
                    break;

                if (msg.event != UFFD_EVENT_PAGEFAULT)
                    continue;

                uint64_t flags = msg.arg.pagefault.flags;
                void *fault_page = (void *)((uintptr_t)msg.arg.pagefault.address &
                                            ~(PROMOTION_UNIT - 1));

                if (!(flags & UFFD_PAGEFAULT_FLAG_WP)) {
                    /* ---------------- MISSING fault ---------------- */
//                    printf("MISSING fault at %p\n", fault_page);

                    /* Wait if some promotion thread already owns this region. */
                    bool inflight = wait_if_inflight(pr, fault_page);

                    if (!inflight) {
                        /* Not covered by any inflight region: do on-demand promotion here. */
                        int rc = promote_region_from_fault(pr, fault_page);
                        if (rc < 0) {
                            fprintf(stderr,
                                    "on-demand promotion from MISSING fault failed for %p: %d\n",
                                    fault_page, rc);
                            /* We still wake the faulting thread; it may fault again. */
                        }
                    }

                    /* Either someone else finished promotion, or we promoted it here. */
                    struct uffdio_range wake = {
                        .start = (unsigned long)fault_page,
                        .len   = PROMOTION_UNIT,
                    };
                    if (ioctl(pr->uffd, UFFDIO_WAKE, &wake) == -1) {
                        perror("UFFDIO_WAKE (missing)");
                    }

  //                  printf("MISSING fault handled (inflight=%d)\n",
  //                         inflight ? 1 : 0);

                } else {
                    /* ---------------- WP fault ---------------- */
//                    printf("WP fault at %p\n", fault_page);

                    /*
                     * Same policy as MISSING:
                     * - If some worker is already promoting this region, just wait.
                     * - If not inflight, do on-demand promotion in the fault handler.
                     */
                    bool inflight = wait_if_inflight(pr, fault_page);

                    if (!inflight) {
                        int rc = promote_region_from_fault(pr, fault_page);
                        if (rc < 0) {
                            fprintf(stderr,
                                    "on-demand promotion from WP fault failed for %p: %d\n",
                                    fault_page, rc);
                            /* After this, we still unprotect + wake so the guest can retry. */
                        }
                    }

                    /* Now drop write-protection so the write can proceed on the promoted page. */
                    if (wp_unprotect_range(pr->uffd, fault_page,
                                           PROMOTION_UNIT) == 0) {
                        g_stats.wp_faults_handled++;
                    } else {
                        perror("wp_unprotect_range");
                    }

                    struct uffdio_range wake = {
                        .start = (unsigned long)fault_page,
                        .len   = PROMOTION_UNIT,
                    };
                    if (ioctl(pr->uffd, UFFDIO_WAKE, &wake) == -1) {
                        perror("UFFDIO_WAKE (wp)");
                    }

//                    printf("WP fault handled (inflight=%d)\n",
//                           inflight ? 1 : 0);
                }
            }
        }
    }

    return NULL;
}


#if 0

static void *fault_handler_thread(void *arg)
{
    PagePromoter *pr = (PagePromoter *)arg;
    struct epoll_event evs[8];

    while (pr->monitor_active) {
        int n = epoll_wait(pr->epoll_fd, evs, 8, 1000); /* 1s timeout */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }
        if (n == 0)
            continue;

        for (int i = 0; i < n; ++i) {
            if (!(evs[i].events & EPOLLIN))
                continue;

            for (;;) {
                struct uffd_msg msg;
                ssize_t r = read(pr->uffd, &msg, sizeof(msg));
                if (r == -1 && errno == EAGAIN)
                    break;
                if (r <= 0)
                    break;

                if (msg.event != UFFD_EVENT_PAGEFAULT)
                    continue;

                uint64_t flags = msg.arg.pagefault.flags;
                void *fault_page = (void *)((uintptr_t)msg.arg.pagefault.address &
                                            ~(PROMOTION_UNIT - 1));

                if (!(flags & UFFD_PAGEFAULT_FLAG_WP)) {
                    /* ---------------- MISSING fault ---------------- */
//                    printf("MISSING fault at %p\n", fault_page);

                    /* Wait if some promotion thread already owns this region. */
                    bool inflight = wait_if_inflight(pr, fault_page);

                    if (!inflight) {
                        /* Not covered by any inflight region: do on-demand promotion here. */
                        int rc = promote_region_from_fault(pr, fault_page);
                        if (rc < 0) {
                            fprintf(stderr,
                                    "on-demand promotion from fault failed for %p: %d\n",
                                    fault_page, rc);
                            /* We still wake the faulting thread; it may fault again. */
                        }
                    }

                    /* Either someone else finished promotion, or we promoted it here. */
                    struct uffdio_range wake = {
                        .start = (unsigned long)fault_page,
                        .len   = PROMOTION_UNIT,
                    };
                    if (ioctl(pr->uffd, UFFDIO_WAKE, &wake) == -1) {
                        perror("UFFDIO_WAKE (missing)");
                    }

//                    printf("MISSING fault handled (inflight=%d)\n", inflight ? 1 : 0);
                } else {
                    /* ---------------- WP fault ---------------- */
//                    printf("WP fault at %p\n", fault_page);

                    /* For WP, we only wait for any in-flight promotion to finish. */
                    (void)wait_if_inflight(pr, fault_page);

                    if (wp_unprotect_range(pr->uffd, fault_page,
                                           PROMOTION_UNIT) == 0) {
                        g_stats.wp_faults_handled++;
                    } else {
                        perror("wp_unprotect_range");
                    }

                    struct uffdio_range wake = {
                        .start = (unsigned long)fault_page,
                        .len   = PROMOTION_UNIT,
                    };
                    if (ioctl(pr->uffd, UFFDIO_WAKE, &wake) == -1) {
                        perror("UFFDIO_WAKE (wp)");
                    }

//                    printf("WP fault handled\n");
                }
            }
        }
    }

    return NULL;
}
#endif


/* --------------------------------------------------------------------- */
/* Single 2MB promotion                                                  */
/* --------------------------------------------------------------------- */
static int promote_single_region(PagePromoter *pr, void *region_addr)
{
    int ret = 0;
    int slot = -1;

    /* 1) Mark this chunk as inflight and enable WP */
    if (inflight_add_wait(pr, region_addr, PROMOTION_UNIT, &slot) < 0) {
        fprintf(stderr, "Failed to allocate inflight slot\n");
        g_stats.promotion_failures++;
        return -1;
    }

    if (wp_protect_range(pr->uffd, region_addr, PROMOTION_UNIT) < 0) {
        fprintf(stderr, "wp_protect_range failed for %p\n", region_addr);
        g_stats.promotion_failures++;
        inflight_mark_done_slot(pr, slot);
        return ERR_WP_FAILED;
    }

    /* 2) Allocate a temporary anonymous 2MB destination */
    void *tmp = mmap(NULL, PROMOTION_UNIT,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                     -1, 0);
    if (tmp == MAP_FAILED) {
        perror("mmap anonymous tmp");
        g_stats.promotion_failures++;

        /* roll back WP / inflight */
        (void)wp_unprotect_range(pr->uffd, region_addr, PROMOTION_UNIT);
        inflight_mark_done_slot(pr, slot);
        return ERR_MMAP_FAILED;
    }

    /* 3) Copy from shadow DevDAX mapping into anon tmp */
    ptrdiff_t offset = (char *)region_addr - (char *)pr->start_addr;
    void *src = (char *)pr->shadow_base + offset;
    fm2_promoter_throttle(PROMOTION_UNIT);
    memcpy(tmp, src, PROMOTION_UNIT);

    /* 4) Atomically replace DevDAX with the anon pages at the original VA */
    void *moved = mremap(tmp, PROMOTION_UNIT, PROMOTION_UNIT,
                         MREMAP_MAYMOVE | MREMAP_FIXED, region_addr);
    if (moved == MAP_FAILED) {
        int err = errno;
        perror("mremap to target region");
        munmap(tmp, PROMOTION_UNIT);

        (void)wp_unprotect_range(pr->uffd, region_addr, PROMOTION_UNIT);
        g_stats.promotion_failures++;
        inflight_mark_done_slot(pr, slot);
        return -err;
    }

    /* 5) Mark inflight done; WP will be cleared lazily on first write */
    inflight_mark_done_slot(pr, slot);

    g_stats.pages_promoted++;
    return ret;
}

/* --------------------------------------------------------------------- */
/* Promotion worker threads                                              */
/* --------------------------------------------------------------------- */
typedef struct {
    PagePromoter *pr;
    const Fm2PromotionOrder *order;
    size_t first_idx;  /* inclusive */
    size_t last_idx;   /* exclusive */
} WorkerArgs;

static int fm2_promotion_order_cmp(const void *lhs, const void *rhs)
{
    const Fm2PromotionOrder *a = lhs;
    const Fm2PromotionOrder *b = rhs;

    if (a->hot != b->hot) {
        return a->hot ? -1 : 1;
    }
    if (a->hot && a->epoch != b->epoch) {
        return a->epoch > b->epoch ? -1 : 1;
    }
    if (a->hot && a->freq != b->freq) {
        return a->freq > b->freq ? -1 : 1;
    }
    return a->index < b->index ? -1 : a->index != b->index;
}

static Fm2PromotionOrder *fm2_build_promotion_order(void *start_addr,
                                                     size_t length,
                                                     size_t total_chunks,
                                                     size_t promotion_offset)
{
    Fm2PromotionOrder *order = calloc(total_chunks, sizeof(*order));
    if (!order) {
        return NULL;
    }

    for (size_t i = 0; i < total_chunks; i++) {
        order[i].index = i;
    }

    if (length <= METADATA_SIZE + sizeof(Fm2HotHdr)) {
        return order;
    }

    Fm2HotHdr *hdr = (Fm2HotHdr *)((char *)start_addr + METADATA_SIZE);
    size_t available = length - METADATA_SIZE - sizeof(*hdr);
    size_t max_entries = available / sizeof(Fm2HotEntry);
    if (hdr->magic != FM2_HOT_MAGIC || hdr->version != FM2_HOT_VERSION ||
        hdr->entry_bytes != FM2_HOT_ENTRY_BYTES || !hdr->cap ||
        hdr->cap > max_entries) {
        fprintf(stderr, "FM2 hotness metadata unavailable; using address order\n");
        return order;
    }

    Fm2HotEntry *entries = (Fm2HotEntry *)(hdr + 1);
    const uint64_t data_base = promotion_offset;
    const uint64_t data_end = data_base +
                              total_chunks * (uint64_t)PROMOTION_UNIT;
    size_t hot_count = 0;
    for (uint32_t i = 0; i < hdr->cap; i++) {
        Fm2HotEntry *entry = &entries[i];
        if (!entry->freq || entry->key_off_2m < data_base ||
            entry->key_off_2m >= data_end ||
            (entry->key_off_2m - data_base) % PROMOTION_UNIT) {
            continue;
        }

        size_t index = (entry->key_off_2m - data_base) /
                       PROMOTION_UNIT;
        order[index].hot = true;
        order[index].epoch = entry->last_epoch;
        order[index].freq = entry->freq;
        hot_count++;
    }

    qsort(order, total_chunks, sizeof(*order), fm2_promotion_order_cmp);
    printf("FM2 FMLift: %zu hot 2 MiB regions prioritized\n", hot_count);
    return order;
}

static void *promotion_worker(void *opaque)
{
    WorkerArgs *wa = (WorkerArgs *)opaque;
    PagePromoter *pr = wa->pr;

    for (size_t idx = wa->first_idx; idx < wa->last_idx; ++idx) {
        size_t chunk_index = wa->order ? wa->order[idx].index : idx;
        void *region_addr =
            (char *)pr->promotion_start + chunk_index * PROMOTION_UNIT;

        struct timeval t0, t1;
        gettimeofday(&t0, NULL);

        int ret = promote_single_region(pr, region_addr);

        gettimeofday(&t1, NULL);
        if (ret < 0) {
            if (ret == ERR_WP_FAILED || ret == ERR_MMAP_FAILED ||
                ret == ERR_MEMORY_ALLOC) {
                fprintf(stderr, "Warning: failed to promote %p, continuing…\n",
                        region_addr);
            } else {
                fprintf(stderr, "Critical error (%d) at region %p, stopping worker\n",
                        ret, region_addr);
                break;
            }
        } else {
            double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                        (t1.tv_usec - t0.tv_usec) / 1000.0;
            (void)ms;
            /* Uncomment if you want timing per chunk:
             * printf("Promoted %p in %.3f ms\n", region_addr, ms);
             */
        }
    }

    return NULL;
}

/* --------------------------------------------------------------------- */
/* Top-level promotion of a single DevDAX VMA                            */
/* --------------------------------------------------------------------- */
static void verify_promotion(void *start_addr, size_t length)
{
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        return;
    }
    printf("\nMemory mappings after promotion:\n");
    char line[512];
    uintptr_t lo = (uintptr_t)start_addr, hi = lo + length;

    while (fgets(line, sizeof(line), fp)) {
        uintptr_t s, e;
        char perms[5], off[17], dev[12], ino[24], path[256] = {0};
        int n = sscanf(line, "%lx-%lx %4s %16s %11s %23s %255[^\n]",
                       &s, &e, perms, off, dev, ino, path);
        if (n >= 6 && ((s >= lo && s < hi) || (e > lo && e <= hi))) {
            printf("  %s", line);
        }
    }
    fclose(fp);
}

int promote_cxl_to_dram(void *start_addr, size_t length)
{
    int rc = 0;

    if (!start_addr || length == 0) {
        fprintf(stderr, "Invalid parameters\n");
        return -EINVAL;
    }

    /* reset global stats for this run */
    memset(&g_stats, 0, sizeof(g_stats));

    PagePromoter pr;
    memset(&pr, 0, sizeof(pr));

    bool devdax = is_devdax_mapping(start_addr);
    printf("Memory type: %s\n", devdax ? "DevDAX" : "Unknown/Other");
    (void)devdax; /* logic assumes DevDAX, but we print for sanity */

    uintptr_t start = (uintptr_t)start_addr;
    uintptr_t end   = start + length;

    start &= ~(uintptr_t)(BASE_PAGE - 1);
    end    = (end + BASE_PAGE - 1) & ~(uintptr_t)(BASE_PAGE - 1);

    pr.start_addr = (void *)start;
    pr.length     = end - start;

    pr.uffd = setup_userfaultfd();
    if (pr.uffd < 0) {
        return ERR_UFFD_CREATE;
    }

    /* epoll setup */
    pr.epoll_fd = epoll_create1(0);
    if (pr.epoll_fd == -1) {
        perror("epoll_create1");
        close(pr.uffd);
        return -1;
    }

    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = pr.uffd,
    };
    if (epoll_ctl(pr.epoll_fd, EPOLL_CTL_ADD, pr.uffd, &ev) == -1) {
        perror("epoll_ctl ADD");
        close(pr.epoll_fd);
        close(pr.uffd);
        return -1;
    }

    pthread_mutex_init(&pr.inflight_mu, NULL);
    pthread_cond_init(&pr.inflight_cv, NULL);

    pr.monitor_active = true;

    /*
     * Shadow mapping: still map the entire DevDAX range so the
     * fault handler can service MISSING faults from this clone.
     * This is independent of which subrange is UFFD-registered.
     */
    pr.shadow_base = map_shadow_dax_region(pr.start_addr, pr.length);
    if (pr.shadow_base == MAP_FAILED) {
        fprintf(stderr, "Failed to create shadow mapping for DevDAX VMA\n");
        close(pr.epoll_fd);
        close(pr.uffd);
        pthread_mutex_destroy(&pr.inflight_mu);
        pthread_cond_destroy(&pr.inflight_cv);
        return ERR_MMAP_FAILED;
    }

    /* Start the fault handler thread */
    if (pthread_create(&pr.monitor_thread, NULL,
                       fault_handler_thread, &pr) != 0) {
        perror("pthread_create (fault handler)");
        munmap(pr.shadow_base, pr.length);
        close(pr.epoll_fd);
        close(pr.uffd);
        pthread_mutex_destroy(&pr.inflight_mu);
        pthread_cond_destroy(&pr.inflight_cv);
        return -1;
    }

    /* Decide the promotion range: skip metadata, then align up to 2MB */
    void *promotion_start = (char *)pr.start_addr + METADATA_SIZE;
    size_t promotion_length = pr.length - METADATA_SIZE;

    uintptr_t ps = (uintptr_t)promotion_start;
    if (ps % PROMOTION_UNIT != 0) {
        ps = (ps + PROMOTION_UNIT - 1) & ~(uintptr_t)(PROMOTION_UNIT - 1);
        promotion_start  = (void *)ps;
        promotion_length = ((char *)pr.start_addr + pr.length) -
                           (char *)promotion_start;
    }

    pr.promotion_start  = promotion_start;
    pr.promotion_length = promotion_length;

    printf("Starting promotion: %zu MB from %p (total VMA %zu MB)\n",
           promotion_length / (1024 * 1024), promotion_start,
           pr.length / (1024 * 1024));

    size_t total_chunks = promotion_length / PROMOTION_UNIT;
    if (total_chunks == 0) {
        printf("Nothing to promote.\n");
        rc = 0;
        goto out;
    }

    /* Jonggyu: FMSync publishes recency/frequency metadata in the shared
     * mapping. FMLift consumes it before falling back to an address-ordered
     * sweep, so the destination working set reaches DRAM first. */
    Fm2PromotionOrder *promotion_order =
        fm2_build_promotion_order(pr.start_addr, pr.length, total_chunks,
                                  (size_t)((char *)promotion_start -
                                           (char *)pr.start_addr));

    /*
     * Sliding 1 GiB window:
     * - Only the current window is registered with UFFD (MISSING+WP).
     * - Outside the window, the DevDAX mapping is untouched (no UFFD),
     *   so reads/writes don't trap to the handler.
     */
    const size_t WINDOW_SIZE = (size_t)1UL << 30; /* 1 GiB */
    size_t chunks_per_window = WINDOW_SIZE / PROMOTION_UNIT;
    if (chunks_per_window == 0) {
        chunks_per_window = 1;  /* paranoid fallback */
    }

    size_t next_chunk = 0;

    while (next_chunk < total_chunks) {
        /* Determine how many chunks in this window */
        size_t window_chunks = total_chunks - next_chunk;
        if (window_chunks > chunks_per_window) {
            window_chunks = chunks_per_window;
        }

        void *window_start = (char *)promotion_start +
                             next_chunk * PROMOTION_UNIT;
        size_t window_length = window_chunks * PROMOTION_UNIT;

//        printf("Activating UFFD window: %zu MB from %p (chunks %zu..%zu)\n",
 //              window_length / (1024 * 1024), window_start,
  //             next_chunk, next_chunk + window_chunks);

        if (register_uffd_range(pr.uffd, window_start, window_length) < 0) {
            fprintf(stderr, "register_uffd_range (window) failed\n");
            rc = ERR_UFFD_REGISTER;
            break;
        }

        /* Multi-threaded worker pool *for this window only* */
        int n_workers = sysconf(_SC_NPROCESSORS_ONLN);
        if (n_workers <= 0) {
            n_workers = 1;
        }
        if ((size_t)n_workers > window_chunks) {
            n_workers = (int)window_chunks;
        }
        if (n_workers > MAX_INFLIGHT) {
            n_workers = MAX_INFLIGHT;
        }

        pthread_t *workers = calloc(n_workers, sizeof(*workers));
        WorkerArgs *wargs  = calloc(n_workers, sizeof(*wargs));
        Fm2PromotionOrder *window_order =
            calloc(window_chunks, sizeof(*window_order));
        if (!workers || !wargs || !window_order) {
            fprintf(stderr, "Failed to allocate worker structures\n");
            free(workers);
            free(wargs);
            free(window_order);

            rc = ERR_MEMORY_ALLOC;
            unregister_uffd_range(pr.uffd, window_start, window_length);
            break;
        }

        /* Preserve the global hot-first order while restricting this pass to
         * the currently registered 1 GiB UFFD window. */
        size_t selected = 0;
        for (size_t i = 0; i < total_chunks; i++) {
            size_t index = promotion_order ? promotion_order[i].index : i;
            if (index >= next_chunk && index < next_chunk + window_chunks) {
                window_order[selected++] = promotion_order ?
                    promotion_order[i] : (Fm2PromotionOrder){ .index = index };
            }
        }
        if (selected != window_chunks) {
            fprintf(stderr, "FM2 promotion order did not cover UFFD window\n");
            free(workers);
            free(wargs);
            free(window_order);
            rc = -EINVAL;
            unregister_uffd_range(pr.uffd, window_start, window_length);
            break;
        }

        size_t base_chunks_per_worker = window_chunks / n_workers;
        size_t rem = window_chunks % n_workers;
        size_t off = 0;  /* offset within this window */

        for (int i = 0; i < n_workers; ++i) {
            size_t count = base_chunks_per_worker +
                           ((size_t)i < rem ? 1 : 0);

            wargs[i].pr        = &pr;
            wargs[i].order     = window_order;
            /* Global chunk indices [next_chunk + off, next_chunk + off + count) */
            wargs[i].first_idx = off;
            wargs[i].last_idx  = off + count;
            off += count;

            if (pthread_create(&workers[i], NULL,
                               promotion_worker, &wargs[i]) != 0) {
                perror("pthread_create (worker)");
                /* Only join the ones that actually started */
                n_workers = i;
                rc = -1;
                break;
            }
        }

        for (int i = 0; i < n_workers; ++i) {
            pthread_join(workers[i], NULL);
        }

        free(workers);
        free(wargs);
        free(window_order);

        /* Deactivate this window: stop trapping faults for it. */
        unregister_uffd_range(pr.uffd, window_start, window_length);

        next_chunk += window_chunks;
    }

    free(promotion_order);

    printf("\nPromotion complete (or aborted on error):\n");
    printf("  Chunks promoted: %lu (%lu MB)\n",
           g_stats.pages_promoted,
           g_stats.pages_promoted * (PROMOTION_UNIT / (1024 * 1024)));
    printf("  Pages skipped: %lu\n", g_stats.pages_skipped);
    printf("  WP faults handled: %lu\n", g_stats.wp_faults_handled);
    printf("  Promotion failures: %lu\n", g_stats.promotion_failures);

out:
    /* Tear down fault handler and resources */
    pr.monitor_active = false;
    pthread_cond_broadcast(&pr.inflight_cv);
    pthread_join(pr.monitor_thread, NULL);

    munmap(pr.shadow_base, pr.length);
    /* Per-window unregisters already done; no global unregister here. */
    close(pr.epoll_fd);
    close(pr.uffd);
    pthread_mutex_destroy(&pr.inflight_mu);
    pthread_cond_destroy(&pr.inflight_cv);

    return rc;
}


#if 0
int promote_cxl_to_dram(void *start_addr, size_t length)
{
    if (!start_addr || length == 0) {
        fprintf(stderr, "Invalid parameters\n");
        return -EINVAL;
    }

    /* reset global stats for this run */
    memset(&g_stats, 0, sizeof(g_stats));

    PagePromoter pr;
    memset(&pr, 0, sizeof(pr));

    bool devdax = is_devdax_mapping(start_addr);
    printf("Memory type: %s\n", devdax ? "DevDAX" : "Unknown/Other");
    (void)devdax; /* logic assumes DevDAX, but we print for sanity */

    uintptr_t start = (uintptr_t)start_addr;
    uintptr_t end   = start + length;

    start &= ~(uintptr_t)(BASE_PAGE - 1);
    end    = (end + BASE_PAGE - 1) & ~(uintptr_t)(BASE_PAGE - 1);

    pr.start_addr = (void *)start;
    pr.length     = end - start;

    pr.uffd = setup_userfaultfd();
    if (pr.uffd < 0) {
        return ERR_UFFD_CREATE;
    }

    /* epoll setup */
    pr.epoll_fd = epoll_create1(0);
    if (pr.epoll_fd == -1) {
        perror("epoll_create1");
        close(pr.uffd);
        return -1;
    }

    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = pr.uffd,
    };
    if (epoll_ctl(pr.epoll_fd, EPOLL_CTL_ADD, pr.uffd, &ev) == -1) {
        perror("epoll_ctl ADD");
        close(pr.epoll_fd);
        close(pr.uffd);
        return -1;
    }

    pthread_mutex_init(&pr.inflight_mu, NULL);
    pthread_cond_init(&pr.inflight_cv, NULL);

    pr.monitor_active = true;

    /* Register the entire VMA for MISSING + WP */
    if (register_uffd_range(pr.uffd, pr.start_addr, pr.length) < 0) {
        fprintf(stderr, "register_uffd_range failed\n");
        close(pr.epoll_fd);
        close(pr.uffd);
        pthread_mutex_destroy(&pr.inflight_mu);
        pthread_cond_destroy(&pr.inflight_cv);
        return ERR_UFFD_REGISTER;
    }

    /* Single shadow mapping for the entire DevDAX range */
    pr.shadow_base = map_shadow_dax_region(pr.start_addr, pr.length);
    if (pr.shadow_base == MAP_FAILED) {
        fprintf(stderr, "Failed to create shadow mapping for DevDAX VMA\n");
        unregister_uffd_range(pr.uffd, pr.start_addr, pr.length);
        close(pr.epoll_fd);
        close(pr.uffd);
        pthread_mutex_destroy(&pr.inflight_mu);
        pthread_cond_destroy(&pr.inflight_cv);
        return ERR_MMAP_FAILED;
    }


    /* Start the fault handler thread */
    if (pthread_create(&pr.monitor_thread, NULL,
                       fault_handler_thread, &pr) != 0) {
        perror("pthread_create (fault handler)");
        munmap(pr.shadow_base, pr.length);
        unregister_uffd_range(pr.uffd, pr.start_addr, pr.length);
        close(pr.epoll_fd);
        close(pr.uffd);
        pthread_mutex_destroy(&pr.inflight_mu);
        pthread_cond_destroy(&pr.inflight_cv);
        return -1;
    }



    /* Decide the promotion range: skip metadata, then align up to 2MB */
    void *promotion_start = (char *)pr.start_addr + METADATA_SIZE;
    size_t promotion_length = pr.length - METADATA_SIZE;

    uintptr_t ps = (uintptr_t)promotion_start;
    if (ps % PROMOTION_UNIT != 0) {
        ps = (ps + PROMOTION_UNIT - 1) & ~(uintptr_t)(PROMOTION_UNIT - 1);
        promotion_start  = (void *)ps;
        promotion_length = ((char *)pr.start_addr + pr.length) -
                           (char *)promotion_start;
    }

    pr.promotion_start  = promotion_start;
    pr.promotion_length = promotion_length;

    printf("Starting promotion: %zu MB from %p (total VMA %zu MB)\n",
           promotion_length / (1024 * 1024), promotion_start,
           pr.length / (1024 * 1024));

    size_t total = promotion_length / PROMOTION_UNIT;
    if (total == 0) {
        printf("Nothing to promote.\n");
        pr.monitor_active = false;
        pthread_cond_broadcast(&pr.inflight_cv);
        pthread_join(pr.monitor_thread, NULL);

        munmap(pr.shadow_base, pr.length);
        unregister_uffd_range(pr.uffd, pr.start_addr, pr.length);
        close(pr.epoll_fd);
        close(pr.uffd);
        pthread_mutex_destroy(&pr.inflight_mu);
        pthread_cond_destroy(&pr.inflight_cv);
        return 0;
    }

    /* Multi-threaded worker pool */
    int n_workers = sysconf(_SC_NPROCESSORS_ONLN);
    if (n_workers <= 0) {
        n_workers = 1;
    }
    if ((size_t)n_workers > total) {
        n_workers = (int)total;
    }
    if (n_workers > MAX_INFLIGHT) {
        n_workers = MAX_INFLIGHT;
    }

    pthread_t *workers = calloc(n_workers, sizeof(*workers));
    WorkerArgs *wargs  = calloc(n_workers, sizeof(*wargs));
    if (!workers || !wargs) {
        fprintf(stderr, "Failed to allocate worker structures\n");
        free(workers);
        free(wargs);

        pr.monitor_active = false;
        pthread_cond_broadcast(&pr.inflight_cv);
        pthread_join(pr.monitor_thread, NULL);

        munmap(pr.shadow_base, pr.length);
        unregister_uffd_range(pr.uffd, pr.start_addr, pr.length);
        close(pr.epoll_fd);
        close(pr.uffd);
        pthread_mutex_destroy(&pr.inflight_mu);
        pthread_cond_destroy(&pr.inflight_cv);
        return ERR_MEMORY_ALLOC;
    }

    size_t base_chunks_per_worker = total / n_workers;
    size_t rem = total % n_workers;
    size_t next = 0;

    for (int i = 0; i < n_workers; ++i) {
        size_t count = base_chunks_per_worker + ((size_t)i < rem ? 1 : 0);

        wargs[i].pr        = &pr;
        wargs[i].first_idx = next;
        wargs[i].last_idx  = next + count;
        next += count;

        if (pthread_create(&workers[i], NULL,
                           promotion_worker, &wargs[i]) != 0) {
            perror("pthread_create (worker)");
            /* Mark fewer workers and break */
            n_workers = i;
            break;
        }
    }

    for (int i = 0; i < n_workers; ++i) {
        pthread_join(workers[i], NULL);
    }

    free(workers);
    free(wargs);

    printf("\nPromotion complete:\n");
    printf("  Chunks promoted: %lu (%lu MB)\n",
           g_stats.pages_promoted,
           g_stats.pages_promoted * (PROMOTION_UNIT / (1024 * 1024)));
    printf("  Pages skipped: %lu\n", g_stats.pages_skipped);
    printf("  WP faults handled: %lu\n", g_stats.wp_faults_handled);
    printf("  Promotion failures: %lu\n", g_stats.promotion_failures);

    /* Debug: dump mappings if you want */
    /* verify_promotion(pr.start_addr, pr.length); */

    pr.monitor_active = false;
    pthread_cond_broadcast(&pr.inflight_cv);
    pthread_join(pr.monitor_thread, NULL);

    munmap(pr.shadow_base, pr.length);
    unregister_uffd_range(pr.uffd, pr.start_addr, pr.length);
    close(pr.epoll_fd);
    close(pr.uffd);
    pthread_mutex_destroy(&pr.inflight_mu);
    pthread_cond_destroy(&pr.inflight_cv);

    return 0;
}
#endif

static int promote_all_cxl_vmas_from_proc_maps(void)
{
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        perror("fopen(/proc/self/maps)");
        return -errno;
    }

    typedef struct {
        uintptr_t start;
        uintptr_t end;
        size_t    len;
        char      path[256];
    } dax_vma_t;

    dax_vma_t *vmas = NULL;
    size_t vma_cap  = 0;
    size_t vma_cnt  = 0;

    char line[512];
    size_t scanned_vmas = 0;

    /* Skip tiny VMAs (e.g., metadata, guard pages, etc.) */
    const size_t MIN_DAX_VMA_MB    = 20;  /* configurable */
    const size_t MIN_DAX_VMA_BYTES = MIN_DAX_VMA_MB * 1024UL * 1024UL;

    while (fgets(line, sizeof(line), fp)) {
        uintptr_t start, end;
        char perms[5], offset[17], dev[12], inode[24], path[256] = {0};

        int n = sscanf(line, "%lx-%lx %4s %16s %11s %23s %255[^\n]",
                       &start, &end, perms, offset, dev, inode, path);
        if (n < 6) {
            continue;
        }

        if (path[0] == '\0') {
            continue;
        }

        if (!strstr(path, "/dev/dax")) {
            continue;
        }

        size_t len = end - start;
        if (len == 0) {
            continue;
        }

        scanned_vmas++;
        printf("Found DevDAX VMA #%zu: %p-%p (%zu MB) %s\n",
               scanned_vmas, (void *)start, (void *)end,
               len / (1024 * 1024), path);

        if (len < MIN_DAX_VMA_BYTES) {
            printf("  -> Skipping (smaller than %zu MB)\n", MIN_DAX_VMA_MB);
            continue;
        }

        /* Grow the array if needed */
        if (vma_cnt == vma_cap) {
            size_t new_cap = vma_cap ? vma_cap * 2 : 8;
            dax_vma_t *tmp = realloc(vmas, new_cap * sizeof(*vmas));
            if (!tmp) {
                perror("realloc(vmas)");
                free(vmas);
                fclose(fp);
                return -ENOMEM;
            }
            vmas    = tmp;
            vma_cap = new_cap;
        }

        vmas[vma_cnt].start = start;
        vmas[vma_cnt].end   = end;
        vmas[vma_cnt].len   = len;
        strncpy(vmas[vma_cnt].path, path, sizeof(vmas[vma_cnt].path) - 1);
        vmas[vma_cnt].path[sizeof(vmas[vma_cnt].path) - 1] = '\0';
        vma_cnt++;
    }

    fclose(fp);

    if (vma_cnt == 0) {
        fprintf(stderr,
                "No /dev/dax VMAs >= %zu MB found in /proc/self/maps; nothing to promote.\n",
                MIN_DAX_VMA_MB);
        free(vmas);
        return -ENOENT;
    }

    printf("Promoting %zu DevDAX VMA(s) (>= %zu MB each).\n",
           vma_cnt, MIN_DAX_VMA_MB);

    int overall_rc = 0;

    for (size_t i = 0; i < vma_cnt; ++i) {
        printf("Promoting DevDAX VMA[%zu]: %p-%p (%zu MB) %s\n",
               i,
               (void *)vmas[i].start,
               (void *)vmas[i].end,
               vmas[i].len / (1024 * 1024),
               vmas[i].path);

        int rc = promote_cxl_to_dram((void *)vmas[i].start, vmas[i].len);
        if (rc < 0) {
            fprintf(stderr,
                    "  -> promote_cxl_to_dram failed for VMA[%zu]: rc=%d\n",
                    i, rc);
            /* keep going, but remember the last error */
            overall_rc = rc;
        }
    }

    free(vmas);
    return overall_rc;
}

#if 0

/* --------------------------------------------------------------------- */
/* Walk /proc/self/maps, find largest /dev/dax VMA, promote it           */
/* --------------------------------------------------------------------- */
static int promote_all_cxl_vmas_from_proc_maps(void)
{
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        perror("fopen(/proc/self/maps)");
        return -errno;
    }

    char line[512];
    size_t vma_count = 0;

    uintptr_t best_start = 0;
    uintptr_t best_end   = 0;
    size_t    best_len   = 0;
    char      best_path[256] = {0};

    while (fgets(line, sizeof(line), fp)) {
        uintptr_t start, end;
        char perms[5], offset[17], dev[12], inode[24], path[256] = {0};

        int n = sscanf(line, "%lx-%lx %4s %16s %11s %23s %255[^\n]",
                       &start, &end, perms, offset, dev, inode, path);
        if (n < 6) {
            continue;
        }

        if (path[0] == '\0') {
            continue;
        }

        if (!strstr(path, "/dev/dax")) {
            continue;
        }

        size_t len = end - start;
        if (len == 0) {
            continue;
        }

        vma_count++;
        printf("Found DevDAX VMA #%zu: %p-%p (%zu MB) %s\n",
               vma_count, (void *)start, (void *)end,
               len / (1024 * 1024), path);

        if (len > best_len) {
            best_len   = len;
            best_start = start;
            best_end   = end;
            strncpy(best_path, path, sizeof(best_path) - 1);
            best_path[sizeof(best_path) - 1] = '\0';
        }
    }

    fclose(fp);

    if (vma_count == 0 || best_len == 0) {
        fprintf(stderr,
                "No /dev/dax VMAs found in /proc/self/maps; nothing to promote.\n");
        return -ENOENT;
    }

    printf("Promoting largest DevDAX VMA: %p-%p (%zu MB) %s\n",
           (void *)best_start, (void *)best_end,
           best_len / (1024 * 1024), best_path);

    int rc = promote_cxl_to_dram((void *)best_start, best_len);
    if (rc < 0) {
        fprintf(stderr, "promote_cxl_to_dram failed for largest VMA: %d\n", rc);
    }

    return rc;
}
#endif
/* --------------------------------------------------------------------- */
/* QEMU entry point (spawns detached thread)                             */
/* --------------------------------------------------------------------- */
typedef struct {
    void *base;
    size_t length;
} Fm2PromotionRequest;

static pthread_mutex_t g_promotion_state_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_promotion_active;

static void *cxl_promote_thread(void *opaque)
{
    Fm2PromotionRequest *request = opaque;
    int ret = request->base && request->length
        ? promote_cxl_to_dram(request->base, request->length)
        : promote_all_cxl_vmas_from_proc_maps();

    free(request);
    if (ret) {
        error_report("CXL promotion thread failed: %d", ret);
    } else {
        error_printf("CXL promotion thread finished successfully\n");
    }

    pthread_mutex_lock(&g_promotion_state_mu);
    g_promotion_active = false;
    pthread_mutex_unlock(&g_promotion_state_mu);
    return NULL;
}

int qemu_promote_vm_cxl_memory(void *vm_cxl_base, size_t vm_cxl_size)
{
    static QemuThread promote_thread;
    Fm2PromotionRequest *request;

    pthread_mutex_lock(&g_promotion_state_mu);
    if (g_promotion_active) {
        /* Jonggyu: reject only overlapping promotions; a completed FMLift
         * run clears this state so a later migration can promote again. */
        pthread_mutex_unlock(&g_promotion_state_mu);
        error_report("CXL promotion already in progress");
        return -1;
    }
    g_promotion_active = true;
    pthread_mutex_unlock(&g_promotion_state_mu);

    request = calloc(1, sizeof(*request));
    if (!request) {
        pthread_mutex_lock(&g_promotion_state_mu);
        g_promotion_active = false;
        pthread_mutex_unlock(&g_promotion_state_mu);
        return -ENOMEM;
    }
    request->base = vm_cxl_base;
    request->length = vm_cxl_size;

    qemu_thread_create(&promote_thread,
                       "cxl-promoter",
                       cxl_promote_thread,
                       request,
                       QEMU_THREAD_DETACHED);

    /* Return immediately so the VM stays responsive */
    return 0;
}

/*
 * Compatibility entry point for the original FM2 HMP command.  Promotion
 * is deliberately delegated to the QEMU entry point above: it owns the
 * detached worker and the /proc/self/maps walk, which keeps this legacy
 * interface from starting a second, competing promotion pass.
 */
void fm2_promoter_start(void *devdax_hva_base, uint64_t bytes)
{
    /* Jonggyu: use the caller's exact VMA when supplied. The old
     * /proc/self/maps autodetection remains available only for callers that
     * explicitly pass an empty range. */
    if (!devdax_hva_base || !bytes) {
        error_report("FM2 promoter requires a non-empty DevDAX range");
        return;
    }

    int ret = qemu_promote_vm_cxl_memory(devdax_hva_base, (size_t)bytes);

    if (ret < 0) {
        error_report("FM2 promoter failed to start: %d", ret);
    }
}
