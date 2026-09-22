/*
 * HMP commands related to migration
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "qemu/osdep.h"
#include "block/qapi.h"
#include "migration/snapshot.h"
#include "monitor/hmp.h"
#include "monitor/monitor.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-migration.h"
#include "qapi/qapi-visit-migration.h"
#include "qapi/qmp/qdict.h"
#include "qapi/string-input-visitor.h"
#include "qapi/string-output-visitor.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/sockets.h"
#include "sysemu/runstate.h"
#include "ui/qemu-spice.h"
#include "sysemu/sysemu.h"
#include "options.h"
#include "migration.h"
#include "ram.h"
#include "fm2_promoter.h"

extern int get_config_value(const char *key);

/* ---- devdax visibility helpers (x86-64) ---- */
#define CXL_FLUSH_H
#include <stdint.h>
#include <sys/mman.h>
#include <string.h>

#ifndef __x86_64__
# error "This flush helper assumes x86-64."
#endif

#define CL_SIZE 64

static inline void cxl_clwb(void *p)
{
    /* clwb [mem]  (opcode 66 0F AE /7) */
    asm volatile(".byte 0x66,0x0f,0xae,0x30" : "+m" (*(char *)p));
}

static inline void cxl_sfence(void)
{
    asm volatile("sfence" ::: "memory");
}

/* Flush [addr, addr+len) cachelines (NO fence). */
static inline void cxl_clwb_range_no_sfence(void *addr, size_t len)
{
    uintptr_t p   = (uintptr_t)addr & ~(CL_SIZE - 1);
    uintptr_t end = (uintptr_t)addr + len;
    for (; p < end; p += CL_SIZE)
        cxl_clwb((void *)p);
}

/* One place to fence when you’re ready to publish. */
static inline void cxl_flush_fence(void)
{
    cxl_sfence();
}
static inline void cxl_flush_range(void *addr, size_t len)
{
    uintptr_t p = (uintptr_t)addr & ~(CL_SIZE - 1);
    uintptr_t end = (uintptr_t)addr + len;
    for (; p < end; p += CL_SIZE)
        cxl_clwb((void *)p);
    cxl_sfence();
}
/* ===== BEGIN: Fast parallel invalidate of a large range (64 GiB ok) ===== */
#include <pthread.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <errno.h>

#ifndef CL_SIZE
#define CL_SIZE 64UL
#endif
#ifdef __x86_64__
static inline int have_clflushopt(void) {
    unsigned eax, ebx, ecx, edx;
    __asm__ volatile("cpuid":"=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(7),"c"(0));
    return (ebx & (1u<<23)) != 0;
}
static inline void do_clflushopt(const void *p) {
    asm volatile("clflushopt (%0)" :: "r"(p) : "memory");
}
static inline void do_clflush(const void *p) {
    asm volatile("clflush (%0)" :: "r"(p) : "memory");
}
static inline void do_sfence(void) { asm volatile("sfence" ::: "memory"); }
#else
#error "x86-64 required"
#endif

typedef struct {
    const uint8_t *base;     /* start (aligned to 64B) */
    size_t         nlines;   /* total cachelines to cover */
    int            tid;      /* thread index [0..nthr-1] */
    int            nthr;     /* total threads */
    int            cpu;      /* pin to this CPU id, or -1 */
    int            use_opt;  /* 1: clflushopt, 0: clflush */
} InvArgs;

static void pin_to_cpu(int cpu)
{
#ifdef __linux__
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        (void)sched_setaffinity(0, sizeof(set), &set); /* best-effort */
    }
#else
    (void)cpu;
#endif
}

/* Strided over cachelines: line i handled by thread (i % nthr) == tid.
   This balances perfectly and avoids per-thread chunk math. */
static void *invalidate_worker_fn(void *opaque)
{
    InvArgs *a = (InvArgs *)opaque;
    pin_to_cpu(a->cpu);

    const size_t stride = (size_t)a->nthr * CL_SIZE;
    const uint8_t *p = a->base + (size_t)a->tid * CL_SIZE;
    const uint8_t *end = a->base + a->nlines * CL_SIZE;

    if (a->use_opt) {
        /* Unroll 8× to reduce loop overhead */
        for (; p + 8*stride <= end; p += 8*stride) {
            do_clflushopt(p + 0*stride);
            do_clflushopt(p + 1*stride);
            do_clflushopt(p + 2*stride);
            do_clflushopt(p + 3*stride);
            do_clflushopt(p + 4*stride);
            do_clflushopt(p + 5*stride);
            do_clflushopt(p + 6*stride);
            do_clflushopt(p + 7*stride);
        }
        for (; p < end; p += stride) do_clflushopt(p);
        __builtin_ia32_sfence();   /* ensure completion of this thread’s flushes */
    } else {
        for (; p + 8*stride <= end; p += 8*stride) {
            do_clflush(p + 0*stride);
            do_clflush(p + 1*stride);
            do_clflush(p + 2*stride);
            do_clflush(p + 3*stride);
            do_clflush(p + 4*stride);
            do_clflush(p + 5*stride);
            do_clflush(p + 6*stride);
            do_clflush(p + 7*stride);
        }
        for (; p < end; p += stride) do_clflush(p);
        /* CLFLUSH is ordered enough for each line, but keep SFENCE for symmetry */
        __builtin_ia32_sfence();
    }
    return NULL;
}

/* Public entry: parallel invalidate [addr, addr+len) */
static void cxl_invalidate_range(void *addr, size_t len)
{
    if (len == 0) return;

    uintptr_t start = ((uintptr_t)addr) & ~(CL_SIZE - 1);
    uintptr_t end   = (uintptr_t)addr + len;
    size_t nlines   = (size_t)((end - start + (CL_SIZE - 1)) / CL_SIZE);
    if (nlines == 0) return;

    int online = get_nprocs();                 /* online logical CPUs */
    if (online < 1) online = 1;

    /* Prefer physical cores over SMT siblings: use half the logicals if >1 SMT. */
    int nthr = online;
    if (online >= 4) nthr = online / 2;        /* EPYC: 2 threads/core → use cores */
    /* Cap threads to number of lines */
    if ((size_t)nthr > nlines) nthr = (int)nlines;
    if (nthr < 1) nthr = 1;

    QemuThread *threads = g_new(QemuThread, nthr);
    InvArgs    *args    = g_new(InvArgs,    nthr);

    const uint8_t *base = (const uint8_t *)start;
    const int use_opt = have_clflushopt();

    for (int i = 0; i < nthr; i++) {
        args[i] = (InvArgs){
            .base    = base,
            .nlines  = nlines,
            .tid     = i,
            .nthr    = nthr,
            .cpu     = i,           /* pin 0..nthr-1; kernel will map to cores */
            .use_opt = use_opt,
        };
        qemu_thread_create(&threads[i], "cxl-inv",
                           invalidate_worker_fn, &args[i],
                           QEMU_THREAD_JOINABLE);
    }
    for (int i = 0; i < nthr; i++) {
        qemu_thread_join(&threads[i]);
    }
    g_free(threads);
    g_free(args);

    /* Final fence after all threads completed (belt-and-suspenders). */
    __builtin_ia32_sfence();
}
/* ===== END: Fast parallel invalidate ===== */

static void migration_global_dump(Monitor *mon)
{
    MigrationState *ms = migrate_get_current();

    monitor_printf(mon, "globals:\n");
    monitor_printf(mon, "store-global-state: %s\n",
                   ms->store_global_state ? "on" : "off");
    monitor_printf(mon, "only-migratable: %s\n",
                   only_migratable ? "on" : "off");
    monitor_printf(mon, "send-configuration: %s\n",
                   ms->send_configuration ? "on" : "off");
    monitor_printf(mon, "send-section-footer: %s\n",
                   ms->send_section_footer ? "on" : "off");
    monitor_printf(mon, "decompress-error-check: %s\n",
                   ms->decompress_error_check ? "on" : "off");
    monitor_printf(mon, "clear-bitmap-shift: %u\n",
                   ms->clear_bitmap_shift);
}

/* If you type "info migrate" in monitor interface, it will trigger this function.
 * Print migration information.
 */
void hmp_info_migrate(Monitor *mon, const QDict *qdict)
{
    MigrationInfo *info;

    info = qmp_query_migrate(NULL);

    migration_global_dump(mon);

    if (info->blocked_reasons) {
        strList *reasons = info->blocked_reasons;
        monitor_printf(mon, "Outgoing migration blocked:\n");
        while (reasons) {
            monitor_printf(mon, "  %s\n", reasons->value);
            reasons = reasons->next;
        }
    }

    if (info->has_status) {
        monitor_printf(mon, "Migration status: %s",
                       MigrationStatus_str(info->status));
        if (info->status == MIGRATION_STATUS_FAILED && info->error_desc) {
            monitor_printf(mon, " (%s)\n", info->error_desc);
        } else {
            monitor_printf(mon, "\n");
        }

        monitor_printf(mon, "total time: %" PRIu64 " ms\n",
                       info->total_time);
        if (info->has_expected_downtime) {
            monitor_printf(mon, "expected downtime: %" PRIu64 " ms\n",
                           info->expected_downtime);
        }
        if (info->has_downtime) {
            monitor_printf(mon, "downtime: %" PRIu64 " ms\n",
                           info->downtime);
        }
        if (info->has_setup_time) {
            monitor_printf(mon, "setup: %" PRIu64 " ms\n",
                           info->setup_time);
        }
    }

    if (info->ram) {
        monitor_printf(mon, "transferred ram: %" PRIu64 " kbytes\n",
                       info->ram->transferred >> 10);
        monitor_printf(mon, "throughput: %0.2f mbps\n",
                       info->ram->mbps);
        monitor_printf(mon, "remaining ram: %" PRIu64 " kbytes\n",
                       info->ram->remaining >> 10);
        monitor_printf(mon, "total ram: %" PRIu64 " kbytes\n",
                       info->ram->total >> 10);
        monitor_printf(mon, "duplicate: %" PRIu64 " pages\n",
                       info->ram->duplicate);
        monitor_printf(mon, "skipped: %" PRIu64 " pages\n",
                       info->ram->skipped);
        monitor_printf(mon, "normal: %" PRIu64 " pages\n",
                       info->ram->normal);
        monitor_printf(mon, "normal bytes: %" PRIu64 " kbytes\n",
                       info->ram->normal_bytes >> 10);
        monitor_printf(mon, "dirty sync count: %" PRIu64 "\n",
                       info->ram->dirty_sync_count);
        monitor_printf(mon, "page size: %" PRIu64 " kbytes\n",
                       info->ram->page_size >> 10);
        monitor_printf(mon, "multifd bytes: %" PRIu64 " kbytes\n",
                       info->ram->multifd_bytes >> 10);
        monitor_printf(mon, "pages-per-second: %" PRIu64 "\n",
                       info->ram->pages_per_second);

        if (info->ram->dirty_pages_rate) {
            monitor_printf(mon, "dirty pages rate: %" PRIu64 " pages\n",
                           info->ram->dirty_pages_rate);
        }
        if (info->ram->postcopy_requests) {
            monitor_printf(mon, "postcopy request count: %" PRIu64 "\n",
                           info->ram->postcopy_requests);
        }
        if (info->ram->precopy_bytes) {
            monitor_printf(mon, "precopy ram: %" PRIu64 " kbytes\n",
                           info->ram->precopy_bytes >> 10);
        }
        if (info->ram->downtime_bytes) {
            monitor_printf(mon, "downtime ram: %" PRIu64 " kbytes\n",
                           info->ram->downtime_bytes >> 10);
        }
        if (info->ram->postcopy_bytes) {
            monitor_printf(mon, "postcopy ram: %" PRIu64 " kbytes\n",
                           info->ram->postcopy_bytes >> 10);
        }
        if (info->ram->dirty_sync_missed_zero_copy) {
            monitor_printf(mon,
                           "Zero-copy-send fallbacks happened: %" PRIu64 " times\n",
                           info->ram->dirty_sync_missed_zero_copy);
        }
    }

    if (info->disk) {
        monitor_printf(mon, "transferred disk: %" PRIu64 " kbytes\n",
                       info->disk->transferred >> 10);
        monitor_printf(mon, "remaining disk: %" PRIu64 " kbytes\n",
                       info->disk->remaining >> 10);
        monitor_printf(mon, "total disk: %" PRIu64 " kbytes\n",
                       info->disk->total >> 10);
    }

    if (info->xbzrle_cache) {
        monitor_printf(mon, "cache size: %" PRIu64 " bytes\n",
                       info->xbzrle_cache->cache_size);
        monitor_printf(mon, "xbzrle transferred: %" PRIu64 " kbytes\n",
                       info->xbzrle_cache->bytes >> 10);
        monitor_printf(mon, "xbzrle pages: %" PRIu64 " pages\n",
                       info->xbzrle_cache->pages);
        monitor_printf(mon, "xbzrle cache miss: %" PRIu64 " pages\n",
                       info->xbzrle_cache->cache_miss);
        monitor_printf(mon, "xbzrle cache miss rate: %0.2f\n",
                       info->xbzrle_cache->cache_miss_rate);
        monitor_printf(mon, "xbzrle encoding rate: %0.2f\n",
                       info->xbzrle_cache->encoding_rate);
        monitor_printf(mon, "xbzrle overflow: %" PRIu64 "\n",
                       info->xbzrle_cache->overflow);
    }

    if (info->compression) {
        monitor_printf(mon, "compression pages: %" PRIu64 " pages\n",
                       info->compression->pages);
        monitor_printf(mon, "compression busy: %" PRIu64 "\n",
                       info->compression->busy);
        monitor_printf(mon, "compression busy rate: %0.2f\n",
                       info->compression->busy_rate);
        monitor_printf(mon, "compressed size: %" PRIu64 " kbytes\n",
                       info->compression->compressed_size >> 10);
        monitor_printf(mon, "compression rate: %0.2f\n",
                       info->compression->compression_rate);
    }

    if (info->has_cpu_throttle_percentage) {
        monitor_printf(mon, "cpu throttle percentage: %" PRIu64 "\n",
                       info->cpu_throttle_percentage);
    }

    if (info->has_dirty_limit_throttle_time_per_round) {
        monitor_printf(mon, "dirty-limit throttle time: %" PRIu64 " us\n",
                       info->dirty_limit_throttle_time_per_round);
    }

    if (info->has_dirty_limit_ring_full_time) {
        monitor_printf(mon, "dirty-limit ring full time: %" PRIu64 " us\n",
                       info->dirty_limit_ring_full_time);
    }

    if (info->has_postcopy_blocktime) {
        monitor_printf(mon, "postcopy blocktime: %u\n",
                       info->postcopy_blocktime);
    }

    if (info->has_postcopy_vcpu_blocktime) {
        Visitor *v;
        char *str;
        v = string_output_visitor_new(false, &str);
        visit_type_uint32List(v, NULL, &info->postcopy_vcpu_blocktime,
                              &error_abort);
        visit_complete(v, &str);
        monitor_printf(mon, "postcopy vcpu blocktime: %s\n", str);
        g_free(str);
        visit_free(v);
    }
    if (info->has_socket_address) {
        SocketAddressList *addr;

        monitor_printf(mon, "socket address: [\n");

        for (addr = info->socket_address; addr; addr = addr->next) {
            char *s = socket_uri(addr->value);
            monitor_printf(mon, "\t%s\n", s);
            g_free(s);
        }
        monitor_printf(mon, "]\n");
    }

    if (info->vfio) {
        monitor_printf(mon, "vfio device transferred: %" PRIu64 " kbytes\n",
                       info->vfio->transferred >> 10);
    }

    qapi_free_MigrationInfo(info);
}

/* "info migrate_capabilities" in QEMU monitor.
 * Check what features are enabled.
 */
void hmp_info_migrate_capabilities(Monitor *mon, const QDict *qdict)
{
    MigrationCapabilityStatusList *caps, *cap;

    caps = qmp_query_migrate_capabilities(NULL);

    if (caps) {
        for (cap = caps; cap; cap = cap->next) {
            monitor_printf(mon, "%s: %s\n",
                           MigrationCapability_str(cap->value->capability),
                           cap->value->state ? "on" : "off");
        }
    }

    qapi_free_MigrationCapabilityStatusList(caps);
}

/* "info migrate_parameters" in QEMU monitor.
 * I need to understand the meaning of those parameters.
 */
void hmp_info_migrate_parameters(Monitor *mon, const QDict *qdict)
{
    MigrationParameters *params;

    params = qmp_query_migrate_parameters(NULL);

    if (params) {
        monitor_printf(mon, "%s: %" PRIu64 " ms\n",
            MigrationParameter_str(MIGRATION_PARAMETER_ANNOUNCE_INITIAL),
            params->announce_initial);
        monitor_printf(mon, "%s: %" PRIu64 " ms\n",
            MigrationParameter_str(MIGRATION_PARAMETER_ANNOUNCE_MAX),
            params->announce_max);
        monitor_printf(mon, "%s: %" PRIu64 "\n",
            MigrationParameter_str(MIGRATION_PARAMETER_ANNOUNCE_ROUNDS),
            params->announce_rounds);
        monitor_printf(mon, "%s: %" PRIu64 " ms\n",
            MigrationParameter_str(MIGRATION_PARAMETER_ANNOUNCE_STEP),
            params->announce_step);
        assert(params->has_compress_level);
        monitor_printf(mon, "%s: %u\n",
            MigrationParameter_str(MIGRATION_PARAMETER_COMPRESS_LEVEL),
            params->compress_level);
        assert(params->has_compress_threads);
        monitor_printf(mon, "%s: %u\n",
            MigrationParameter_str(MIGRATION_PARAMETER_COMPRESS_THREADS),
            params->compress_threads);
        assert(params->has_compress_wait_thread);
        monitor_printf(mon, "%s: %s\n",
            MigrationParameter_str(MIGRATION_PARAMETER_COMPRESS_WAIT_THREAD),
            params->compress_wait_thread ? "on" : "off");
        assert(params->has_decompress_threads);
        monitor_printf(mon, "%s: %u\n",
            MigrationParameter_str(MIGRATION_PARAMETER_DECOMPRESS_THREADS),
            params->decompress_threads);
        assert(params->has_throttle_trigger_threshold);
        monitor_printf(mon, "%s: %u\n",
            MigrationParameter_str(MIGRATION_PARAMETER_THROTTLE_TRIGGER_THRESHOLD),
            params->throttle_trigger_threshold);
        assert(params->has_cpu_throttle_initial);
        monitor_printf(mon, "%s: %u\n",
            MigrationParameter_str(MIGRATION_PARAMETER_CPU_THROTTLE_INITIAL),
            params->cpu_throttle_initial);
        assert(params->has_cpu_throttle_increment);
        monitor_printf(mon, "%s: %u\n",
            MigrationParameter_str(MIGRATION_PARAMETER_CPU_THROTTLE_INCREMENT),
            params->cpu_throttle_increment);
        assert(params->has_cpu_throttle_tailslow);
        monitor_printf(mon, "%s: %s\n",
            MigrationParameter_str(MIGRATION_PARAMETER_CPU_THROTTLE_TAILSLOW),
            params->cpu_throttle_tailslow ? "on" : "off");
        assert(params->has_max_cpu_throttle);
        monitor_printf(mon, "%s: %u\n",
            MigrationParameter_str(MIGRATION_PARAMETER_MAX_CPU_THROTTLE),
            params->max_cpu_throttle);
        assert(params->tls_creds);
        monitor_printf(mon, "%s: '%s'\n",
            MigrationParameter_str(MIGRATION_PARAMETER_TLS_CREDS),
            params->tls_creds);
        assert(params->tls_hostname);
        monitor_printf(mon, "%s: '%s'\n",
            MigrationParameter_str(MIGRATION_PARAMETER_TLS_HOSTNAME),
            params->tls_hostname);
        assert(params->has_max_bandwidth);
        monitor_printf(mon, "%s: %" PRIu64 " bytes/second\n",
            MigrationParameter_str(MIGRATION_PARAMETER_MAX_BANDWIDTH),
            params->max_bandwidth);
        assert(params->has_avail_switchover_bandwidth);
        monitor_printf(mon, "%s: %" PRIu64 " bytes/second\n",
            MigrationParameter_str(MIGRATION_PARAMETER_AVAIL_SWITCHOVER_BANDWIDTH),
            params->avail_switchover_bandwidth);
        assert(params->has_downtime_limit);
        monitor_printf(mon, "%s: %" PRIu64 " ms\n",
            MigrationParameter_str(MIGRATION_PARAMETER_DOWNTIME_LIMIT),
            params->downtime_limit);
        assert(params->has_x_checkpoint_delay);
        monitor_printf(mon, "%s: %u ms\n",
            MigrationParameter_str(MIGRATION_PARAMETER_X_CHECKPOINT_DELAY),
            params->x_checkpoint_delay);
        assert(params->has_block_incremental);
        monitor_printf(mon, "%s: %s\n",
            MigrationParameter_str(MIGRATION_PARAMETER_BLOCK_INCREMENTAL),
            params->block_incremental ? "on" : "off");
        monitor_printf(mon, "%s: %u\n",
            MigrationParameter_str(MIGRATION_PARAMETER_MULTIFD_CHANNELS),
            params->multifd_channels);
        monitor_printf(mon, "%s: %s\n",
            MigrationParameter_str(MIGRATION_PARAMETER_MULTIFD_COMPRESSION),
            MultiFDCompression_str(params->multifd_compression));
        assert(params->has_zero_page_detection);
        monitor_printf(mon, "%s: %s\n",
            MigrationParameter_str(MIGRATION_PARAMETER_ZERO_PAGE_DETECTION),
            qapi_enum_lookup(&ZeroPageDetection_lookup,
                params->zero_page_detection));
        monitor_printf(mon, "%s: %" PRIu64 " bytes\n",
            MigrationParameter_str(MIGRATION_PARAMETER_XBZRLE_CACHE_SIZE),
            params->xbzrle_cache_size);
        monitor_printf(mon, "%s: %" PRIu64 "\n",
            MigrationParameter_str(MIGRATION_PARAMETER_MAX_POSTCOPY_BANDWIDTH),
            params->max_postcopy_bandwidth);
        monitor_printf(mon, "%s: '%s'\n",
            MigrationParameter_str(MIGRATION_PARAMETER_TLS_AUTHZ),
            params->tls_authz);

        if (params->has_block_bitmap_mapping) {
            const BitmapMigrationNodeAliasList *bmnal;

            monitor_printf(mon, "%s:\n",
                           MigrationParameter_str(
                               MIGRATION_PARAMETER_BLOCK_BITMAP_MAPPING));

            for (bmnal = params->block_bitmap_mapping;
                 bmnal;
                 bmnal = bmnal->next)
            {
                const BitmapMigrationNodeAlias *bmna = bmnal->value;
                const BitmapMigrationBitmapAliasList *bmbal;

                monitor_printf(mon, "  '%s' -> '%s'\n",
                               bmna->node_name, bmna->alias);

                for (bmbal = bmna->bitmaps; bmbal; bmbal = bmbal->next) {
                    const BitmapMigrationBitmapAlias *bmba = bmbal->value;

                    monitor_printf(mon, "    '%s' -> '%s'\n",
                                   bmba->name, bmba->alias);
                }
            }
        }

        monitor_printf(mon, "%s: %" PRIu64 " ms\n",
        MigrationParameter_str(MIGRATION_PARAMETER_X_VCPU_DIRTY_LIMIT_PERIOD),
        params->x_vcpu_dirty_limit_period);

        monitor_printf(mon, "%s: %" PRIu64 " MB/s\n",
            MigrationParameter_str(MIGRATION_PARAMETER_VCPU_DIRTY_LIMIT),
            params->vcpu_dirty_limit);

        assert(params->has_mode);
        monitor_printf(mon, "%s: %s\n",
            MigrationParameter_str(MIGRATION_PARAMETER_MODE),
            qapi_enum_lookup(&MigMode_lookup, params->mode));
    }

    qapi_free_MigrationParameters(params);
}

void hmp_loadvm(Monitor *mon, const QDict *qdict)
{
    RunState saved_state = runstate_get();

    const char *name = qdict_get_str(qdict, "name");
    Error *err = NULL;

    vm_stop(RUN_STATE_RESTORE_VM);

    if (load_snapshot(name, NULL, false, NULL, &err)) {
        load_snapshot_resume(saved_state);
    }

    hmp_handle_error(mon, err);
}

void hmp_savevm(Monitor *mon, const QDict *qdict)
{
    Error *err = NULL;

    save_snapshot(qdict_get_try_str(qdict, "name"),
                  true, NULL, false, NULL, &err);
    hmp_handle_error(mon, err);
}

void hmp_delvm(Monitor *mon, const QDict *qdict)
{
    Error *err = NULL;
    const char *name = qdict_get_str(qdict, "name");

    delete_snapshot(name, false, NULL, &err);
    hmp_handle_error(mon, err);
}

void hmp_migrate_cancel(Monitor *mon, const QDict *qdict)
{
    qmp_migrate_cancel(NULL);
}

void hmp_migrate_continue(Monitor *mon, const QDict *qdict)
{
    Error *err = NULL;
    const char *state = qdict_get_str(qdict, "state");
    int val = qapi_enum_parse(&MigrationStatus_lookup, state, -1, &err);

    if (val >= 0) {
        qmp_migrate_continue(val, &err);
    }

    hmp_handle_error(mon, err);
}

void hmp_migrate_incoming(Monitor *mon, const QDict *qdict)
{
    Error *err = NULL;
    const char *uri = qdict_get_str(qdict, "uri");
    MigrationChannelList *caps = NULL;
    g_autoptr(MigrationChannel) channel = NULL;

    if (!migrate_uri_parse(uri, &channel, &err)) {
        goto end;
    }
    QAPI_LIST_PREPEND(caps, g_steal_pointer(&channel));

    qmp_migrate_incoming(NULL, true, caps, &err);
    qapi_free_MigrationChannelList(caps);

end:
    hmp_handle_error(mon, err);
}



/* === add once (top of file) === */
#ifndef TRIGGER_PATH
#define TRIGGER_PATH "/sys/kernel/wbinvd_all/trigger"
#endif

static int write_sysfs_literal(const char *path, const char *s) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    ssize_t need = (ssize_t)strlen(s);
    ssize_t n = write(fd, s, need);
    int rc = (n == need) ? 0 : -errno;
    close(fd);
    return rc;
}

static int trigger_wbinvd_all(void) {
    /* try both without and with newline; many sysfs attrs accept either */
    int rc = write_sysfs_literal(TRIGGER_PATH, "1");
    if (rc) rc = write_sysfs_literal(TRIGGER_PATH, "1\n");
    return rc;  /* 0 on success, -errno on failure */
}

void *shm_ptr = NULL;

// in your mmap_activate() destination path after you compute `shm_ptr`:
#define META_STATE_LENGTH     1048576ULL     /* 1 MiB */
#define HOT_PAGE_STATE_LENGTH 9437184ULL     /* ~9 MiB */
#define RAM_FILE_BASE         (META_STATE_LENGTH + HOT_PAGE_STATE_LENGTH)


void hmp_migrate_incoming_shm_setup(Monitor *mon, const QDict *qdict)
{
    (void)qdict_get_str(qdict, "uri"); /* ignored */

    uint64_t shm_size = qdict_get_int(qdict, "value"); // memory size, GB.
    shm_size *= 1024ull * 1024ull * 1024ull;
//    const char *DEV_PATH              = get_config_value("CXL_DEV_PATH");
//    const off_t DEV_OFF = get_config_value("CXL_BASE_OFF_BYTES");
    const char  *DEV_PATH = "/dev/dax1.0";
    const off_t  DEV_OFF  = (off_t)0;

    int shm_fd = open(DEV_PATH, O_RDWR | O_CLOEXEC);
    if (shm_fd == -1) {
        perror("open(/dev/dax)"); assert(0);
        exit(EXIT_FAILURE);
    }

    shm_ptr = mmap(NULL, shm_size, PROT_READ|PROT_WRITE, MAP_SYNC|MAP_SHARED_VALIDATE|MAP_POPULATE, shm_fd, 0);
    close(shm_fd);
    if (shm_ptr == NULL)
        printf("shm mapped failed\n");
    if (shm_ptr == MAP_FAILED) {
        perror("mmap failed");
        exit(EXIT_FAILURE);
    }
    printf("pointer = %p\n", shm_ptr);
/*
    int rc = trigger_wbinvd_all();
    if (rc) {
            error_report("WBINVD trigger failed: %s (path=%s). "
                 "Run QEMU as root or chmod this sysfs node.",
                 strerror(-rc), TRIGGER_PATH);
        printf("failed faild faild\n");
        cxl_invalidate_range(shm_ptr, MIN(shm_size, 32ull*1024*1024)); // e.g., 32 MiB header
    }

*/
    /* Reader-side freshness: blow away any local CPU cachelines covering the map. */
    cxl_invalidate_range(shm_ptr, shm_size);
//    mprotect(shm_ptr, shm_size, PROT_NONE);     // block all access pre-switchover
    printf("in hmp_migrate_incoming_setup\n");
}
void hmp_migrate_promotion_shm(Monitor *mon, const QDict *qdict)
{
    (void)qdict_get_str(qdict, "uri"); /* ignored */
    uint64_t shm_size = qdict_get_int(qdict, "value"); // memory size, GB.
    shm_size *= 1024ull * 1024ull * 1024ull;

    printf("before promoter start, %p, %lu\n", shm_ptr, shm_size);
    /* Jonggyu: keep the DevDAX VMA mapped. FMLift now receives this exact
     * range and atomically replaces its 2 MiB subranges with DRAM mappings. */
    fm2_promoter_start(shm_ptr, shm_size);
//    fm2_promoter_uffd_start(shm_ptr, shm_size);
//    fm2_promoter_swap_start_from_window(shm_ptr, shm_size);

    printf("after promoter start\n");
}

/* Jonggyu: expose the paper's FMSync/FMLift controls through HMP while
 * keeping the existing shared-memory migration commands as compatibility
 * aliases for start and switchover. */
void hmp_migrate_FMSync_start(Monitor *mon, const QDict *qdict)
{
    hmp_shm_migrate(mon, qdict);
}

void hmp_migrate_FMSync_freqctrl(Monitor *mon, const QDict *qdict)
{
    uint64_t interval_us = qdict_get_int(qdict, "value");
    fm2_fmsync_set_frequency(interval_us);
    monitor_printf(mon, "FM2 FMSync interval set to %" PRIu64 " us\n",
                   interval_us);
}

void hmp_migrate_FMSync_bandctrl(Monitor *mon, const QDict *qdict)
{
    uint64_t bandwidth = qdict_get_int(qdict, "value");
    fm2_fmsync_set_bandwidth(bandwidth);
    monitor_printf(mon, "FM2 FMSync bandwidth set to %" PRIu64 " B/s\n",
                   bandwidth);
}

void hmp_migrate_FMSync_switchover(Monitor *mon, const QDict *qdict)
{
    hmp_shm_migrate_switchover(mon, qdict);
}

void hmp_migrate_FMLift_start(Monitor *mon, const QDict *qdict)
{
    hmp_migrate_promotion_shm(mon, qdict);
}

void hmp_migrate_FMLift_bandctrl(Monitor *mon, const QDict *qdict)
{
    uint64_t bandwidth = qdict_get_int(qdict, "value");
    int ret = fm2_promoter_set_bandwidth(bandwidth);
    if (ret < 0) {
        monitor_printf(mon, "FM2 FMLift bandwidth update failed: %d\n", ret);
        return;
    }
    monitor_printf(mon, "FM2 FMLift bandwidth set to %" PRIu64 " B/s\n",
                   bandwidth);
}


void hmp_migrate_incoming_shm(Monitor *mon, const QDict *qdict)
{
    Error *err = NULL;
    (void)qdict_get_str(qdict, "uri"); /* ignored */

    uint64_t shm_size = qdict_get_int(qdict, "value"); // memory size, GB.
    shm_size *= 1024ull * 1024ull * 1024ull;
/*
    const char  *DEV_PATH = "/dev/dax1.0";
    const off_t  DEV_OFF  = (off_t)0;

    int shm_fd = open(DEV_PATH, O_RDWR | O_CLOEXEC);
    if (shm_fd == -1) {
        perror("open(/dev/dax)"); assert(0);
        exit(EXIT_FAILURE);
    }

    void *shm_ptr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SYNC|MAP_SHARED_VALIDATE, shm_fd, DEV_OFF);
    close(shm_fd);
    if (shm_ptr == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }

    int rc = trigger_wbinvd_all();
    if (rc) {
            error_report("WBINVD trigger failed: %s (path=%s). "
                 "Run QEMU as root or chmod this sysfs node.",
                 strerror(-rc), TRIGGER_PATH);
        printf("failed faild faild\n");
        cxl_invalidate_range(shm_ptr, MIN(shm_size, 32ull*1024*1024)); // e.g., 32 MiB header
    }
*/

//    cxl_invalidate_range(shm_ptr, shm_size);
  //  mprotect(shm_ptr, shm_size, PROT_READ|PROT_WRITE);     // block all access pre-switchover
    printf("in hmp_migrate_incoming_shm %p\n", shm_ptr);
    qmp_migrate_incoming_shm(shm_ptr, shm_size, &err);
//    munmap(shm_ptr, shm_size);
    /* Note: if qmp_migrate_incoming_shm() repeatedly reads the region while the
     * source continues writing, you should invalidate only the chunks you read
     * each iteration (before the read) rather than the whole mapping every time. */
}


void hmp_migrate_recover(Monitor *mon, const QDict *qdict)
{
    Error *err = NULL;
    const char *uri = qdict_get_str(qdict, "uri");

    qmp_migrate_recover(uri, &err);

    hmp_handle_error(mon, err);
}

void hmp_migrate_pause(Monitor *mon, const QDict *qdict)
{
    Error *err = NULL;

    qmp_migrate_pause(&err);

    hmp_handle_error(mon, err);
}


void hmp_migrate_set_capability(Monitor *mon, const QDict *qdict)
{
    const char *cap = qdict_get_str(qdict, "capability");
    bool state = qdict_get_bool(qdict, "state");
    Error *err = NULL;
    MigrationCapabilityStatusList *caps = NULL;
    MigrationCapabilityStatus *value;
    int val;

    val = qapi_enum_parse(&MigrationCapability_lookup, cap, -1, &err);
    if (val < 0) {
        goto end;
    }

    value = g_malloc0(sizeof(*value));
    value->capability = val;
    value->state = state;
    QAPI_LIST_PREPEND(caps, value);
    qmp_migrate_set_capabilities(caps, &err);
    qapi_free_MigrationCapabilityStatusList(caps);

end:
    hmp_handle_error(mon, err);
}

void hmp_migrate_set_parameter(Monitor *mon, const QDict *qdict)
{
    const char *param = qdict_get_str(qdict, "parameter");
    const char *valuestr = qdict_get_str(qdict, "value");
    Visitor *v = string_input_visitor_new(valuestr);
    MigrateSetParameters *p = g_new0(MigrateSetParameters, 1);
    uint64_t valuebw = 0;
    uint64_t cache_size;
    Error *err = NULL;
    int val, ret;

    val = qapi_enum_parse(&MigrationParameter_lookup, param, -1, &err);
    if (val < 0) {
        goto cleanup;
    }

    switch (val) {
    case MIGRATION_PARAMETER_COMPRESS_LEVEL:
        p->has_compress_level = true;
        visit_type_uint8(v, param, &p->compress_level, &err);
        break;
    case MIGRATION_PARAMETER_COMPRESS_THREADS:
        p->has_compress_threads = true;
        visit_type_uint8(v, param, &p->compress_threads, &err);
        break;
    case MIGRATION_PARAMETER_COMPRESS_WAIT_THREAD:
        p->has_compress_wait_thread = true;
        visit_type_bool(v, param, &p->compress_wait_thread, &err);
        break;
    case MIGRATION_PARAMETER_DECOMPRESS_THREADS:
        p->has_decompress_threads = true;
        visit_type_uint8(v, param, &p->decompress_threads, &err);
        break;
    case MIGRATION_PARAMETER_THROTTLE_TRIGGER_THRESHOLD:
        p->has_throttle_trigger_threshold = true;
        visit_type_uint8(v, param, &p->throttle_trigger_threshold, &err);
        break;
    case MIGRATION_PARAMETER_CPU_THROTTLE_INITIAL:
        p->has_cpu_throttle_initial = true;
        visit_type_uint8(v, param, &p->cpu_throttle_initial, &err);
        break;
    case MIGRATION_PARAMETER_CPU_THROTTLE_INCREMENT:
        p->has_cpu_throttle_increment = true;
        visit_type_uint8(v, param, &p->cpu_throttle_increment, &err);
        break;
    case MIGRATION_PARAMETER_CPU_THROTTLE_TAILSLOW:
        p->has_cpu_throttle_tailslow = true;
        visit_type_bool(v, param, &p->cpu_throttle_tailslow, &err);
        break;
    case MIGRATION_PARAMETER_MAX_CPU_THROTTLE:
        p->has_max_cpu_throttle = true;
        visit_type_uint8(v, param, &p->max_cpu_throttle, &err);
        break;
    case MIGRATION_PARAMETER_TLS_CREDS:
        p->tls_creds = g_new0(StrOrNull, 1);
        p->tls_creds->type = QTYPE_QSTRING;
        visit_type_str(v, param, &p->tls_creds->u.s, &err);
        break;
    case MIGRATION_PARAMETER_TLS_HOSTNAME:
        p->tls_hostname = g_new0(StrOrNull, 1);
        p->tls_hostname->type = QTYPE_QSTRING;
        visit_type_str(v, param, &p->tls_hostname->u.s, &err);
        break;
    case MIGRATION_PARAMETER_TLS_AUTHZ:
        p->tls_authz = g_new0(StrOrNull, 1);
        p->tls_authz->type = QTYPE_QSTRING;
        visit_type_str(v, param, &p->tls_authz->u.s, &err);
        break;
    case MIGRATION_PARAMETER_MAX_BANDWIDTH:
        p->has_max_bandwidth = true;
        /*
         * Can't use visit_type_size() here, because it
         * defaults to Bytes rather than Mebibytes.
         */
        ret = qemu_strtosz_MiB(valuestr, NULL, &valuebw);
        if (ret < 0 || valuebw > INT64_MAX
            || (size_t)valuebw != valuebw) {
            error_setg(&err, "Invalid size %s", valuestr);
            break;
        }
        p->max_bandwidth = valuebw;
        break;
    case MIGRATION_PARAMETER_AVAIL_SWITCHOVER_BANDWIDTH:
        p->has_avail_switchover_bandwidth = true;
        ret = qemu_strtosz_MiB(valuestr, NULL, &valuebw);
        if (ret < 0 || valuebw > INT64_MAX
            || (size_t)valuebw != valuebw) {
            error_setg(&err, "Invalid size %s", valuestr);
            break;
        }
        p->avail_switchover_bandwidth = valuebw;
        break;
    case MIGRATION_PARAMETER_DOWNTIME_LIMIT:
        p->has_downtime_limit = true;
        visit_type_size(v, param, &p->downtime_limit, &err);
        break;
    case MIGRATION_PARAMETER_X_CHECKPOINT_DELAY:
        p->has_x_checkpoint_delay = true;
        visit_type_uint32(v, param, &p->x_checkpoint_delay, &err);
        break;
    case MIGRATION_PARAMETER_BLOCK_INCREMENTAL:
        p->has_block_incremental = true;
        visit_type_bool(v, param, &p->block_incremental, &err);
        break;
    case MIGRATION_PARAMETER_MULTIFD_CHANNELS:
        p->has_multifd_channels = true;
        visit_type_uint8(v, param, &p->multifd_channels, &err);
        break;
    case MIGRATION_PARAMETER_MULTIFD_COMPRESSION:
        p->has_multifd_compression = true;
        visit_type_MultiFDCompression(v, param, &p->multifd_compression,
                                      &err);
        break;
    case MIGRATION_PARAMETER_MULTIFD_ZLIB_LEVEL:
        p->has_multifd_zlib_level = true;
        visit_type_uint8(v, param, &p->multifd_zlib_level, &err);
        break;
    case MIGRATION_PARAMETER_MULTIFD_ZSTD_LEVEL:
        p->has_multifd_zstd_level = true;
        visit_type_uint8(v, param, &p->multifd_zstd_level, &err);
        break;
    case MIGRATION_PARAMETER_ZERO_PAGE_DETECTION:
        p->has_zero_page_detection = true;
        visit_type_ZeroPageDetection(v, param, &p->zero_page_detection, &err);
        break;
    case MIGRATION_PARAMETER_XBZRLE_CACHE_SIZE:
        p->has_xbzrle_cache_size = true;
        if (!visit_type_size(v, param, &cache_size, &err)) {
            break;
        }
        if (cache_size > INT64_MAX || (size_t)cache_size != cache_size) {
            error_setg(&err, "Invalid size %s", valuestr);
            break;
        }
        p->xbzrle_cache_size = cache_size;
        break;
    case MIGRATION_PARAMETER_MAX_POSTCOPY_BANDWIDTH:
        p->has_max_postcopy_bandwidth = true;
        visit_type_size(v, param, &p->max_postcopy_bandwidth, &err);
        break;
    case MIGRATION_PARAMETER_ANNOUNCE_INITIAL:
        p->has_announce_initial = true;
        visit_type_size(v, param, &p->announce_initial, &err);
        break;
    case MIGRATION_PARAMETER_ANNOUNCE_MAX:
        p->has_announce_max = true;
        visit_type_size(v, param, &p->announce_max, &err);
        break;
    case MIGRATION_PARAMETER_ANNOUNCE_ROUNDS:
        p->has_announce_rounds = true;
        visit_type_size(v, param, &p->announce_rounds, &err);
        break;
    case MIGRATION_PARAMETER_ANNOUNCE_STEP:
        p->has_announce_step = true;
        visit_type_size(v, param, &p->announce_step, &err);
        break;
    case MIGRATION_PARAMETER_BLOCK_BITMAP_MAPPING:
        error_setg(&err, "The block-bitmap-mapping parameter can only be set "
                   "through QMP");
        break;
    case MIGRATION_PARAMETER_X_VCPU_DIRTY_LIMIT_PERIOD:
        p->has_x_vcpu_dirty_limit_period = true;
        visit_type_size(v, param, &p->x_vcpu_dirty_limit_period, &err);
        break;
    case MIGRATION_PARAMETER_VCPU_DIRTY_LIMIT:
        p->has_vcpu_dirty_limit = true;
        visit_type_size(v, param, &p->vcpu_dirty_limit, &err);
        break;
    case MIGRATION_PARAMETER_MODE:
        p->has_mode = true;
        visit_type_MigMode(v, param, &p->mode, &err);
        break;
    default:
        assert(0);
    }

    if (err) {
        goto cleanup;
    }

    qmp_migrate_set_parameters(p, &err);

 cleanup:
    qapi_free_MigrateSetParameters(p);
    visit_free(v);
    hmp_handle_error(mon, err);
}

/* `migrate_start_postcopy` in monitor.
 * start the postcopy migration.
 */
void hmp_migrate_start_postcopy(Monitor *mon, const QDict *qdict)
{
    Error *err = NULL;
    qmp_migrate_start_postcopy(&err);
    hmp_handle_error(mon, err);
}

#ifdef CONFIG_REPLICATION
void hmp_x_colo_lost_heartbeat(Monitor *mon, const QDict *qdict)
{
    Error *err = NULL;

    qmp_x_colo_lost_heartbeat(&err);
    hmp_handle_error(mon, err);
}
#endif

typedef struct HMPMigrationStatus {
    QEMUTimer *timer;
    Monitor *mon;
} HMPMigrationStatus;

static void hmp_migrate_status_cb(void *opaque)
{
    HMPMigrationStatus *status = opaque;
    MigrationInfo *info;

    info = qmp_query_migrate(NULL);
    if (!info->has_status || info->status == MIGRATION_STATUS_ACTIVE ||
        info->status == MIGRATION_STATUS_SETUP) {
        if (info->disk) {
            int progress;

            if (info->disk->remaining) {
                progress = info->disk->transferred * 100 / info->disk->total;
            } else {
                progress = 100;
            }

            monitor_printf(status->mon, "Completed %d %%\r", progress);
            monitor_flush(status->mon);
        }

        timer_mod(status->timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
    } else {
        if (migrate_block()) {
            monitor_printf(status->mon, "\n");
        }
        if (info->error_desc) {
            error_report("%s", info->error_desc);
        }
        monitor_resume(status->mon);
        timer_free(status->timer);
        g_free(status);
    }

    qapi_free_MigrationInfo(info);
}

/* "migrate tcp:10.10.1.1" in QEMU monitor.
 * entrance of migration logic.
 */ 
void hmp_migrate(Monitor *mon, const QDict *qdict)
{
    bool detach = qdict_get_try_bool(qdict, "detach", false);
    bool blk = qdict_get_try_bool(qdict, "blk", false);
    bool inc = qdict_get_try_bool(qdict, "inc", false);
    bool resume = qdict_get_try_bool(qdict, "resume", false);
    const char *uri = qdict_get_str(qdict, "uri"); // the destination address.

    Error *err = NULL;
    g_autoptr(MigrationChannelList) caps = NULL;
    g_autoptr(MigrationChannel) channel = NULL;

    if (inc) {
        warn_report("option '-i' is deprecated;"
                    " use blockdev-mirror with NBD instead");
    }

    if (blk) {
        warn_report("option '-b' is deprecated;"
                    " use blockdev-mirror with NBD instead");
    }

    if (!migrate_uri_parse(uri, &channel, &err)) {
        hmp_handle_error(mon, err);
        return;
    }
    QAPI_LIST_PREPEND(caps, g_steal_pointer(&channel));

    qmp_migrate(NULL, true, caps, !!blk, blk, !!inc, inc,
                 false, false, true, resume, &err);
    if (hmp_handle_error(mon, err)) {
        return;
    }

    if (!detach) {
        HMPMigrationStatus *status;

        if (monitor_suspend(mon) < 0) {
            monitor_printf(mon, "terminal does not allow synchronous "
                           "migration, continuing detached\n");
            return;
        }

        status = g_malloc0(sizeof(*status));
        status->mon = mon;
        status->timer = timer_new_ms(QEMU_CLOCK_REALTIME, hmp_migrate_status_cb,
                                          status);
        timer_mod(status->timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
    }
}


void hmp_shm_migrate(Monitor *mon, const QDict *qdict)
{
    (void)qdict_get_try_str(qdict, "uri"); /* ignored: kept for compat */
    uint64_t size_gb     = qdict_get_int(qdict, "value");    /* size in GB */
    uint64_t duration_us = qdict_get_int(qdict, "duration"); /* microseconds */

    uint64_t size_bytes = size_gb * 1024ull * 1024ull * 1024ull;
//    const char *DEV_PATH              = get_config_value("CXL_DEV_PATH");
//    const off_t DEV_OFF = get_config_value("CXL_BASE_OFF_BYTES");

    const char  *DEV_PATH    = "/dev/dax1.2";
    const off_t  DEV_OFF     = 0;
//    const off_t  DEV_OFF     = 64 GiB(off_t)(64ULL << 30);   /* 64 GiB window base on 303 */
//    const off_t  DEV_OFF     = 68719476736;
    Error *err = NULL;

    int dax_fd = open(DEV_PATH, O_RDWR | O_CLOEXEC);
    if (dax_fd < 0) {
        perror("open(devdax)");
        exit(EXIT_FAILURE);
    }

    int mmap_flags = MAP_SHARED; // | MAP_POPULATE;

    void *dax_ptr = mmap(NULL, size_bytes, PROT_READ | PROT_WRITE,
                         MAP_SYNC | MAP_SHARED_VALIDATE, dax_fd, (off_t)DEV_OFF);
    if (dax_ptr == MAP_FAILED) {
        /* Fallback if MAP_SYNC unsupported */
        dax_ptr = mmap(NULL, size_bytes, PROT_READ | PROT_WRITE,
                       mmap_flags, dax_fd, (off_t)DEV_OFF);
    }
    if (dax_ptr == MAP_FAILED) {
        perror("mmap(devdax)");
        close(dax_fd);
        exit(EXIT_FAILURE);
    }
    memset(dax_ptr, 0, size_bytes);
    madvise(dax_ptr, size_bytes, MADV_HUGEPAGE);
    qmp_shm_migrate(dax_ptr, size_bytes, duration_us, &err);

    if (err) {
        fprintf(stderr, "hmp_shm_migrate: migration failed\n");
        munmap(dax_ptr, size_bytes);
        close(dax_fd);
        exit(EXIT_FAILURE);
    }
    close(dax_fd);
}


/* Zezhou: shm_migrate_switchover.
 */ 
void hmp_shm_migrate_switchover(Monitor *mon, const QDict *qdict)
{
    qmp_shm_migrate_switchover();
    return;
}



void migrate_set_capability_completion(ReadLineState *rs, int nb_args,
                                       const char *str)
{
    size_t len;

    len = strlen(str);
    readline_set_completion_index(rs, len);
    if (nb_args == 2) {
        int i;
        for (i = 0; i < MIGRATION_CAPABILITY__MAX; i++) {
            readline_add_completion_of(rs, str, MigrationCapability_str(i));
        }
    } else if (nb_args == 3) {
        readline_add_completion_of(rs, str, "on");
        readline_add_completion_of(rs, str, "off");
    }
}

void migrate_set_parameter_completion(ReadLineState *rs, int nb_args,
                                      const char *str)
{
    size_t len;

    len = strlen(str);
    readline_set_completion_index(rs, len);
    if (nb_args == 2) {
        int i;
        for (i = 0; i < MIGRATION_PARAMETER__MAX; i++) {
            readline_add_completion_of(rs, str, MigrationParameter_str(i));
        }
    }
}

static void vm_completion(ReadLineState *rs, const char *str)
{
    size_t len;
    BlockDriverState *bs;
    BdrvNextIterator it;

    GRAPH_RDLOCK_GUARD_MAINLOOP();

    len = strlen(str);
    readline_set_completion_index(rs, len);

    for (bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
        SnapshotInfoList *snapshots, *snapshot;
        bool ok = false;

        if (bdrv_can_snapshot(bs)) {
            ok = bdrv_query_snapshot_info_list(bs, &snapshots, NULL) == 0;
        }
        if (!ok) {
            continue;
        }

        snapshot = snapshots;
        while (snapshot) {
            readline_add_completion_of(rs, str, snapshot->value->name);
            readline_add_completion_of(rs, str, snapshot->value->id);
            snapshot = snapshot->next;
        }
        qapi_free_SnapshotInfoList(snapshots);
    }

}

void delvm_completion(ReadLineState *rs, int nb_args, const char *str)
{
    if (nb_args == 2) {
        vm_completion(rs, str);
    }
}

void loadvm_completion(ReadLineState *rs, int nb_args, const char *str)
{
    if (nb_args == 2) {
        vm_completion(rs, str);
    }
}
