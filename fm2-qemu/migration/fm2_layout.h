#ifndef QEMU_MIGRATION_FM2_LAYOUT_H
#define QEMU_MIGRATION_FM2_LAYOUT_H

#include <stdint.h>

/* Persistent layout of the shared Device-DAX migration area. */
#define FM2_META_STATE_SIZE      (1ULL * 1024 * 1024)
#define FM2_HOT_REGION_SIZE      (9ULL * 1024 * 1024)
#define FM2_RAM_FILE_OFFSET      (FM2_META_STATE_SIZE + FM2_HOT_REGION_SIZE)

#define FM2_HOT_V1_MAGIC         0x484F5431u /* "HOT1" */
#define FM2_HOT_V1_VERSION       1
#define FM2_HOT_MAGIC            0x484F5432u /* "HOT2" */
#define FM2_HOT_VERSION          2
#define FM2_HOT_KEY_SHIFT        21
#define FM2_HOT_ENTRY_BYTES      16
#define FM2_PROMOTION_UNIT       (1ULL << FM2_HOT_KEY_SHIFT)

#define FM2_HOT_HEADER_BYTES     64
#define FM2_HOT_HASH_BYTES       (4ULL * 1024 * 1024)
#define FM2_HOT_HASH_OFFSET      FM2_HOT_HEADER_BYTES
#define FM2_HOT_LIST_OFFSET      (FM2_HOT_HASH_OFFSET + FM2_HOT_HASH_BYTES)

enum Fm2HotState {
    FM2_HOT_STATE_HASH_ACTIVE = 1,
    FM2_HOT_STATE_LIST_BUILDING = 2,
    FM2_HOT_STATE_LIST_COMMITTED = 3,
};

/* Legacy format retained so a new destination can consume HOT1 sources. */
typedef struct Fm2HotHdrV1 {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t epoch;
    uint32_t cap;
    uint32_t entry_bytes;
    uint32_t n_items;
    uint32_t pad;
} Fm2HotHdrV1;

typedef struct Fm2HotHdr {
    uint32_t magic;
    uint16_t version;
    uint16_t state;
    uint32_t epoch;
    uint32_t entry_bytes;
    uint32_t hash_capacity;
    uint32_t hash_items;
    uint32_t list_capacity;
    uint32_t list_count;
    uint32_t flags;
    uint64_t hash_offset;
    uint64_t list_offset;
    uint64_t generation;
} Fm2HotHdr;

typedef struct Fm2HotEntry {
    uint64_t key_off_2m;
    uint32_t last_epoch;
    uint16_t freq;
    uint16_t pad;
} Fm2HotEntry;

_Static_assert(sizeof(Fm2HotEntry) == FM2_HOT_ENTRY_BYTES,
               "FM2 hot entry layout mismatch");
_Static_assert(sizeof(Fm2HotHdr) == FM2_HOT_HEADER_BYTES,
               "FM2 HOT2 header layout mismatch");
_Static_assert(FM2_RAM_FILE_OFFSET % FM2_PROMOTION_UNIT == 0,
               "FM2 RAM file offset must be 2 MiB aligned");

#endif
