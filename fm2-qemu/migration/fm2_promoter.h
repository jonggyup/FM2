// fm2_promoter_swap.h
#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * Public FM2 promotion entry point used by the HMP migration command.
 * The current implementation discovers active /dev/dax VMAs itself.
 */
void fm2_promoter_start(void *devdax_hva_base, uint64_t bytes);

/* Asynchronous implementation used by the FM2 QEMU integration. */
int qemu_promote_vm_cxl_memory(void *vm_cxl_base, size_t vm_cxl_size);

/* Jonggyu: FMLift's independent CXL-to-DRAM bandwidth control. */
int fm2_promoter_set_bandwidth(uint64_t bytes_per_sec);

void fm2_promoter_swap_start_from_window(void *dax_window_base, uint64_t dax_window_size_bytes);
void fm2_promoter_swap_start_autodetect(void);
