#pragma once

#include <stdint.h>

constexpr unsigned int SPORE_BOOT_MODULE_PATH_MAX = 96u;
constexpr unsigned int SPORE_BOOT_CPU_MAX = 64u;

constexpr int SPORE_MEMMAP_USABLE = 1;

struct spore_memmap_entry {
  uint64_t base;
  uint64_t length;
  uint32_t type;
  uint32_t reserved;
};

struct spore_boot_module {
  uint64_t phys_addr;
  uint64_t size;
  char path[SPORE_BOOT_MODULE_PATH_MAX];
};

struct limine_mp_response;

struct spore_boot_info {
  uint64_t hhdm_offset;
  uint64_t realtime_epoch_sec;
  const struct spore_memmap_entry *memmap;
  uint32_t memmap_count;
  const struct spore_boot_module *modules;
  uint32_t module_count;
  struct limine_mp_response *mp;
};

const struct spore_boot_info *limine_boot_info(void);
