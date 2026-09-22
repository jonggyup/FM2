# FM2 Functionality and Paper-to-Source Map

This document explains where the paper's mechanisms are implemented and how
the modified kernel and QEMU cooperate.

## End-to-end control flow

1. FMSync repeatedly observes guest memory dirtiness and copies changed data
   into the shared CXL shadow while recording 2 MiB locality metadata.
2. FMFlush stops the source VM, completes the remaining dirty work, and
   publishes the CXL image and metadata.
3. The destination resumes from the shared CXL image.
4. FMLift promotes CXL-backed regions into local DRAM, starting with hot
   regions and then sweeping the remaining regions.

The primary runtime loop is:

```text
migration_iteration_run_shm()
  -> ram_save_iterate_shm()
       -> KVM_FMSYNC_GET_DIRTY_LOG_HUGE
       -> copy dirty 2 MiB regions to the CXL shadow
       -> update epoch/frequency metadata
  -> qatomic switchover request
  -> migration_completion_shm()
       -> ram_save_complete_shm()
            -> base-page finalization and cache publication
  -> destination FMLift HMP command
       -> fm2_promoter_start()
            -> userfaultfd + write protection + mremap promotion
```

## FMSync: background shadow synchronization

Paper contribution: maintain a CXL shadow copy while the VM runs, use coarse
2 MiB dirty tracking during normal operation, and record locality metadata for
later promotion.

QEMU implementation:

- [`migration/migration.c`](fm2-qemu/migration/migration.c)
  - `migration_iteration_run_shm()` is the continuous FMSync controller.
  - It invokes one shared-memory RAM iteration, waits for the configured
    interval, and checks the atomic switchover request.
  - `qmp_shm_migrate()` creates the shared-memory migration thread.
- [`migration/ram.c`](fm2-qemu/migration/ram.c)
  - `ram_save_iterate_shm()` is the FMSync data path.
  - It requests huge-page dirty state, copies dirty regions to the CXL shadow,
    clears the corresponding bits, and updates the recency/frequency table.
  - `HotHdr` and `HotEntry` store the epoch, frequency, and 2 MiB file offset.
  - `fm2_fmsync_set_frequency()` and `fm2_fmsync_set_bandwidth()` pace the
    background iterations.
- [`migration/migration-hmp-cmds.c`](fm2-qemu/migration/migration-hmp-cmds.c)
  - Implements the HMP wrappers for FMSync start, frequency control, and
    bandwidth control.

Kernel implementation:

- [`include/uapi/linux/kvm.h`](fm2-kernel/include/uapi/linux/kvm.h)
  - Adds `KVM_FMSYNC_GET_DIRTY_LOG_HUGE`.
  - Adds `KVM_FMSYNC_GET_DIRTY_LOG_BASE_WITH_SPLIT`.
- [`virt/kvm/kvm_main.c`](fm2-kernel/virt/kvm/kvm_main.c)
  - Adds a private `fmsync_dirty_bitmap` per memory slot.
  - Adds the FM2 ioctl path, including slot validation, bitmap allocation,
    MMU locking, TLB flushing, and copying the bitmap to QEMU.
- [`arch/x86/kvm/mmu/tdp_mmu.c`](fm2-kernel/arch/x86/kvm/mmu/tdp_mmu.c)
  - Walks TDP leaf SPTEs.
  - Emits one bit per 2 MiB region for huge-page mode.
  - Emits one bit per 4 KiB page after huge-page splitting for base mode.
  - Clears dirty/write state so the next FMSync epoch observes new writes.

## FMFlush: consistent handoff

Paper contribution: stop the source VM, drain the remaining dirty state, flush
the CXL image and locality metadata, then hand control to the destination.

Implementation locations:

- [`migration/migration.c`](fm2-qemu/migration/migration.c)
  - `migration_completion_shm()` handles the shared-memory completion path and
    controller notifications.
  - `migration_completion_precopy_shm()` stops the VM and invokes the shared
    save-completion hook.
- [`migration/ram.c`](fm2-qemu/migration/ram.c)
  - `ram_save_complete_shm()` copies remaining dirty data at base-page
    granularity, updates the hotness table, and publishes data with cache-line
    writeback/fences.
- [`migration/savevm.c`](fm2-qemu/migration/savevm.c)
  - Provides the shared-memory save header/setup and completion hooks used by
    the migration layer.

This path corresponds to FMFlush's consistency boundary, not to ordinary
QEMU network migration completion.

## FMLift: hot-first CXL-to-DRAM promotion

Paper contribution: resume from CXL, promote the hot working set first, then
gradually sweep the remaining CXL-backed memory while bounding promotion
bandwidth.

Implementation locations:

- [`migration/fm2_promoter.c`](fm2-qemu/migration/fm2_promoter.c)
  - `fm2_promoter_start()` accepts the exact DevDAX VMA supplied by the HMP
    path.
  - `promote_cxl_to_dram()` establishes userfaultfd and a shadow DevDAX
    mapping.
  - Each 2 MiB region is write-protected, copied into anonymous DRAM, and
    atomically replaced with `mremap(..., MREMAP_FIXED, ...)`.
  - FMSync's epoch/frequency table is read from the shared metadata region.
    Hot regions are ordered before the address-ordered cold sweep within each
    registered promotion window.
  - `fm2_promoter_set_bandwidth()` limits CXL-to-DRAM copy throughput.
  - Promotion state is cleared after completion, allowing a later run in the
    same QEMU process.
- [`migration/migration-hmp-cmds.c`](fm2-qemu/migration/migration-hmp-cmds.c)
  - `migrate_FMLift_start` starts promotion.
  - `migrate_FMLift_bandctrl` configures the promotion bandwidth cap.
- [`hmp-commands.hx`](fm2-qemu/hmp-commands.hx)
  - Registers the six paper-named FMSync/FMLift controls.

## DevDAX userfaultfd kernel delta

The paper's FMLift mechanism requires userfaultfd write-protection faults on a
DevDAX mapping. Upstream Linux 6.8 does not provide the FM2 path used here.

- [`include/linux/userfaultfd_k.h`](fm2-kernel/include/linux/userfaultfd_k.h)
  - Allows the relevant userfaultfd write-protection capability for DAX VMAs.
- [`mm/mprotect.c`](fm2-kernel/mm/mprotect.c)
  - Contains the FM2 diagnostic path confirming write protection reaches DAX
    PTEs.
- [`arch/x86/kvm/mmu/mmu.c`](fm2-kernel/arch/x86/kvm/mmu/mmu.c) and
  [`arch/x86/kvm/mmu/tdp_mmu.h`](fm2-kernel/arch/x86/kvm/mmu/tdp_mmu.h)
  - Connect the generic KVM FM2 ioctl to the x86 TDP implementation.

## HMP command mapping

| HMP command | Implementation | Paper role |
| --- | --- | --- |
| `migrate_FMSync_start` | `hmp_shm_migrate()` | Start FMSync |
| `migrate_FMSync_freqctrl` | `fm2_fmsync_set_frequency()` | FMSync interval |
| `migrate_FMSync_bandctrl` | `fm2_fmsync_set_bandwidth()` | FMSync bandwidth |
| `migrate_FMSync_switchover` | `qmp_shm_migrate_switchover()` | FMFlush handoff request |
| `migrate_FMLift_start` | `fm2_promoter_start()` | Start destination promotion |
| `migrate_FMLift_bandctrl` | `fm2_promoter_set_bandwidth()` | FMLift bandwidth |
