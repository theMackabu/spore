#pragma once

#include "boot_info.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PAGE_SIZE = 4096 };

struct pmm_stats {
  uint64_t alloc_attempts;
  uint64_t alloc_successes;
  uint64_t alloc_failures;
  uint64_t bitmap_words_scanned;
};

void pmm_init(uint64_t hhdm_offset, const struct spore_memmap_entry *memmap, uint32_t memmap_count);
uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_zero_page(void);
void pmm_free_page(uint64_t pa);
bool pmm_share_page(uint64_t pa);
bool pmm_is_last_ref(uint64_t pa);
uint16_t pmm_refcount(uint64_t pa);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
struct pmm_stats pmm_get_stats(void);
