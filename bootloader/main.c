#include "bootloader.h"

EFI_SYSTEM_TABLE *st;
EFI_BOOT_SERVICES *bs;
EFI_FILE_PROTOCOL *root;
uint64_t (*page_table_pool)[PT_ENTRIES];
uint32_t page_table_pool_used;
uint32_t page_table_pool_count;

typedef void (*kernel_entry_t)(const struct spore_boot_info *);

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
  st = system_table;
  bs = system_table->boot_services;
  set_boot_timeout_zero();
  uefi_puts(u"spore-boot: loading\r\n");

  if (current_el() != 1) {
    uefi_puts(u"spore-boot: expected EL1\r\n");
    return EFI_LOAD_ERROR;
  }
  EFI_STATUS status = open_root(image);
  if (EFI_ERROR(status)) {
    uefi_puts(u"spore-boot: root fs failed\r\n");
    return status;
  }

  struct loaded_file kernel_file;
  uint64_t kernel_phys_base = 0;
  uint64_t kernel_virt_base = 0;
  uint64_t kernel_span = 0;
  uint64_t entry = 0;
  status = load_kernel(&kernel_file, &kernel_phys_base, &kernel_virt_base, &kernel_span, &entry);
  if (EFI_ERROR(status)) {
    uefi_puts(u"spore-boot: kernel load failed\r\n");
    return status;
  }

  struct spore_boot_module *modules = NULL;
  struct spore_memmap_entry *memmap = NULL;
  EFI_MEMORY_DESCRIPTOR *efi_map = NULL;
  struct spore_boot_info *boot = NULL;
  if (EFI_ERROR(alloc_pages(pages_for(sizeof(struct spore_boot_module) * MAX_MODULES), (void **)&modules)) ||
      EFI_ERROR(alloc_pages(pages_for(sizeof(struct spore_memmap_entry) * MAX_MEMMAP), (void **)&memmap)) ||
      EFI_ERROR(alloc_pages(16, (void **)&efi_map)) || EFI_ERROR(alloc_pages(1, (void **)&boot))) {
    uefi_puts(u"spore-boot: metadata alloc failed\r\n");
    return EFI_LOAD_ERROR;
  }

  uint32_t module_count = 0;
  status = load_modules(modules, &module_count);
  if (EFI_ERROR(status)) {
    uefi_puts(u"spore-boot: module load failed\r\n");
    return status;
  }

  uint64_t highest_usable = 0;
  status = memory_map_highest_usable(efi_map, 16 * PAGE_SIZE, &highest_usable);
  if (EFI_ERROR(status) || highest_usable == 0) {
    uefi_puts(u"spore-boot: memory map failed\r\n");
    return EFI_ERROR(status) ? status : EFI_LOAD_ERROR;
  }
  uint64_t hhdm_size = align_up(highest_usable, 0x40000000ull);
  if (hhdm_size < HHDM_MIN_SIZE) { hhdm_size = HHDM_MIN_SIZE; }

  uint64_t hhdm_gib = hhdm_size / 0x40000000ull;
  uint64_t pool_pages = hhdm_gib + 16;
  if (pool_pages < PAGE_TABLE_POOL_MIN_PAGES) { pool_pages = PAGE_TABLE_POOL_MIN_PAGES; }
  if (pool_pages > UINT32_MAX) { return EFI_LOAD_ERROR; }
  page_table_pool_count = (uint32_t)pool_pages;
  if (EFI_ERROR(alloc_pages(pool_pages, (void **)&page_table_pool))) {
    uefi_puts(u"spore-boot: page table pool alloc failed\r\n");
    return EFI_LOAD_ERROR;
  }
  page_table_pool_used = 0;

  uint64_t ttbr1 = 0;
  if (!build_page_tables(kernel_phys_base, kernel_virt_base, kernel_span, entry, hhdm_size, &ttbr1)) {
    uefi_puts(u"spore-boot: page tables failed\r\n");
    return EFI_LOAD_ERROR;
  }

  boot->magic = SPORE_BOOT_MAGIC;
  boot->version = SPORE_BOOT_VERSION;
  boot->memmap_phys = (uint64_t)(uintptr_t)memmap;
  boot->module_count = module_count;
  boot->modules_phys = (uint64_t)(uintptr_t)modules;
  boot->hhdm_offset = HHDM_OFFSET;
  boot->kernel_phys_base = kernel_phys_base;
  boot->kernel_virt_base = kernel_virt_base;
  boot->uart_phys = PL011_PHYS;
  boot->realtime_epoch_sec = read_realtime_epoch();

  status = final_memory_map(memmap, &boot->memmap_count, efi_map, 16 * PAGE_SIZE, image);
  if (EFI_ERROR(status)) { return status; }

  uart_puts("spore-boot: exited boot services\n");
  install_ttbr1(ttbr1);
  kernel_entry_t kernel = (kernel_entry_t)(uintptr_t)entry;
  kernel(boot);
  for (;;) {
    __asm__ volatile("wfe");
  }
}
