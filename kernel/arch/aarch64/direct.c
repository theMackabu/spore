#include "arch/aarch64/platform.h"
#include "init/boot_info.h"
#include <stddef.h>

enum { RAM_BASE = 0x40000000, KERNEL_PA = 0x40200000, PAYLOAD_PA = 0x40400000 };
constexpr uint64_t DIRECT_MAP = 0xffff000000000000ull;
constexpr uint64_t PAGE = 4096;
constexpr uint64_t ACCESSED_NORMAL = (1u << 10) | (3u << 8);
constexpr uint64_t READ_ONLY = 1u << 7;
constexpr uint64_t NO_USER_EXEC = 1ull << 54;
constexpr uint64_t NO_EXEC = (1ull << 53) | NO_USER_EXEC;

alignas(4096) uint64_t direct_low_root[512], direct_high_root[512], direct_empty_root[512];
alignas(4096) static uint64_t identity_l1[512], identity_l2[512];
alignas(4096) static uint64_t kernel_l1[512], kernel_l2[512], kernel_l3[512];
alignas(4096) static uint64_t ram_l1[512], ram_l2[512], image_alias[512];
struct handoff {
  uint64_t ram_bytes, payload_bytes, epoch;
};
constexpr uintptr_t HANDOFF_PA = PAYLOAD_PA - sizeof(struct handoff);
extern char direct_start[], __rodata_start[], __data_start[], __bss_end[];

static uint64_t table(uint64_t *entries) {
  return (uintptr_t)entries | 3;
}

void direct_build_tables(void) {
  const struct handoff *args = (const void *)HANDOFF_PA;
  if (args->ram_bytes < (32ull << 20) || args->ram_bytes > (1ull << 30) || (args->ram_bytes & ((2ull << 20) - 1))) {
    arch_poweroff();
  }
  direct_low_root[0] = table(identity_l1);
  identity_l1[1] = table(identity_l2);
  identity_l2[1] = KERNEL_PA | ACCESSED_NORMAL | READ_ONLY | NO_USER_EXEC | 1;
  direct_high_root[0] = table(ram_l1);
  ram_l1[1] = table(ram_l2);
  ram_l2[1] = table(image_alias);
  for (uint64_t block = 2; block < (args->ram_bytes >> 21); ++block) {
    ram_l2[block] = (RAM_BASE + (block << 21)) | ACCESSED_NORMAL | NO_EXEC | 1;
  }
  direct_high_root[511] = table(kernel_l1);
  kernel_l1[510] = table(kernel_l2);
  kernel_l2[0] = table(kernel_l3);
  for (uintptr_t pa = KERNEL_PA; pa < PAYLOAD_PA; pa += PAGE) {
    uint64_t perm = pa < (uintptr_t)__data_start ? READ_ONLY : 0;
    size_t page = (pa - KERNEL_PA) / PAGE;
    image_alias[page] = pa | perm | NO_EXEC | ACCESSED_NORMAL | 3;
    if (pa >= (uintptr_t)__bss_end) { continue; }
    perm |= pa < (uintptr_t)__rodata_start ? NO_USER_EXEC : NO_EXEC;
    kernel_l3[page] = pa | perm | ACCESSED_NORMAL | 3;
  }
}

const struct spore_boot_info *direct_boot_info(void) {
  const struct handoff *args = (const void *)(uintptr_t)(DIRECT_MAP + HANDOFF_PA);
  static struct spore_memmap_entry free_ram[2];
  static struct spore_boot_module app = {.phys_addr = PAYLOAD_PA, .path = "/spore.app"};
  static struct spore_boot_info boot = {
    .hhdm_offset = DIRECT_MAP, .modules = &app, .module_count = 1, .memmap = free_ram};
  if (!args->payload_bytes || args->payload_bytes > args->ram_bytes - (PAYLOAD_PA - RAM_BASE)) { arch_poweroff(); }
  uint64_t after_app = PAYLOAD_PA + args->payload_bytes;
  uint64_t kernel_end = KERNEL_PA + ((uintptr_t)__bss_end - (uintptr_t)direct_start);
  free_ram[0] = (struct spore_memmap_entry){kernel_end, HANDOFF_PA - kernel_end, SPORE_MEMMAP_USABLE, 0};
  free_ram[1] = (struct spore_memmap_entry){after_app, RAM_BASE + args->ram_bytes - after_app, SPORE_MEMMAP_USABLE, 0};
  app.size = args->payload_bytes;
  boot.memmap_count = 2;
  boot.realtime_epoch_sec = args->epoch;
  return &boot;
}
