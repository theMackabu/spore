#pragma once

#include "../kernel/src/include/boot_info.h"
#include "uefi.h"

#define PAGE_SIZE 4096ull
#define PT_ENTRIES 512u
#define HHDM_OFFSET 0xffff000000000000ull
#define HHDM_SIZE 0x180000000ull
#define PL011_PHYS 0x09000000ull
#define MAX_MODULES 64u
#define MAX_MEMMAP 256u
#define MODULE_TEXT_MAX 8192u
#define PAGE_TABLE_POOL_PAGES 64u

struct loaded_file {
  void *data;
  uint64_t size;
  uint64_t phys;
};

extern EFI_SYSTEM_TABLE *st;
extern EFI_BOOT_SERVICES *bs;
extern EFI_FILE_PROTOCOL *root;
extern uint64_t (*page_table_pool)[PT_ENTRIES];
extern uint32_t page_table_pool_used;

void *memset(void *dst, int c, uint64_t n);
void *memcpy(void *dst, const void *src, uint64_t n);
uint64_t strlen8(const char *s);
void uefi_puts(const CHAR16 *s);
void set_boot_timeout_zero(void);
uint64_t read_realtime_epoch(void);
void uart_puts(const char *s);
uint64_t align_down(uint64_t value, uint64_t align);
uint64_t align_up(uint64_t value, uint64_t align);
uint64_t pages_for(uint64_t size);
EFI_STATUS alloc_pages(uint64_t pages, void **out);

EFI_STATUS open_root(EFI_HANDLE image);
EFI_STATUS read_file(CHAR16 *path, struct loaded_file *out);
void ascii_to_efi_path(const char *src, CHAR16 *dst, uint64_t cap);
EFI_STATUS load_modules(struct spore_boot_module *modules, uint32_t *count);
EFI_STATUS load_kernel(struct loaded_file *kernel_file, uint64_t *kernel_phys_base, uint64_t *kernel_virt_base,
                       uint64_t *kernel_span, uint64_t *entry);

uint64_t current_el(void);
void install_ttbr1(uint64_t root_pa);
int build_page_tables(uint64_t kernel_phys_base, uint64_t kernel_virt_base, uint64_t kernel_span, uint64_t entry,
                      uint64_t *ttbr1_out);

EFI_STATUS final_memory_map(struct spore_memmap_entry *out, uint32_t *out_count, EFI_MEMORY_DESCRIPTOR *efi_map,
                            UINTN efi_map_capacity, EFI_HANDLE image);
