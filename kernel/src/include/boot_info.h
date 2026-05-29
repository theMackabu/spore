#pragma once

#include <stdint.h>

#define SPORE_BOOT_MAGIC 0x53504f5245424f4full
#define SPORE_BOOT_VERSION 3u
#define SPORE_BOOT_MODULE_PATH_MAX 96u
#define SPORE_BOOT_CPU_MAX 64u

enum spore_memmap_type {
  SPORE_MEMMAP_USABLE = 1,
  SPORE_MEMMAP_RESERVED = 2,
  SPORE_MEMMAP_ACPI_RECLAIMABLE = 3,
  SPORE_MEMMAP_ACPI_NVS = 4,
  SPORE_MEMMAP_MMIO = 5,
  SPORE_MEMMAP_BOOTLOADER_RECLAIMABLE = 6,
};

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

enum spore_cpu_flags {
  SPORE_CPU_PRESENT = 1u << 0,
  SPORE_CPU_BOOT = 1u << 1,
};

struct spore_cpu_entry {
  uint64_t mpidr;
  uint32_t flags;
  uint32_t reserved;
};

struct spore_boot_info {
  uint64_t magic;
  uint32_t version;
  uint32_t memmap_count;
  uint64_t memmap_phys;
  uint32_t module_count;
  uint32_t cpu_count;
  uint64_t modules_phys;
  uint64_t cpu_entries_phys;
  uint64_t hhdm_offset;
  uint64_t kernel_phys_base;
  uint64_t kernel_virt_base;
  uint64_t uart_phys;
  uint64_t realtime_epoch_sec;
};
