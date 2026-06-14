#include "bootloader.h"

EFI_SYSTEM_TABLE *st;
EFI_BOOT_SERVICES *bs;
EFI_FILE_PROTOCOL *root;
uint64_t (*page_table_pool)[PT_ENTRIES];
uint32_t page_table_pool_used;
uint32_t page_table_pool_count;

typedef void (*kernel_entry_t)(const struct spore_boot_info *);

static uint32_t framebuffer_format_from_info(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info) {
  if (info == NULL) { return SPORE_FB_FORMAT_NONE; }
  if (info->pixel_format == PixelRedGreenBlueReserved8BitPerColor) { return SPORE_FB_FORMAT_RGBX8888; }
  if (info->pixel_format == PixelBlueGreenRedReserved8BitPerColor) { return SPORE_FB_FORMAT_BGRX8888; }
  if (info->pixel_format == PixelBitMask) {
    if (info->pixel_information.red_mask == 0x000000ffu && info->pixel_information.green_mask == 0x0000ff00u &&
        info->pixel_information.blue_mask == 0x00ff0000u) {
      return SPORE_FB_FORMAT_RGBX8888;
    }
    if (info->pixel_information.red_mask == 0x00ff0000u && info->pixel_information.green_mask == 0x0000ff00u &&
        info->pixel_information.blue_mask == 0x000000ffu) {
      return SPORE_FB_FORMAT_BGRX8888;
    }
  }
  return SPORE_FB_FORMAT_NONE;
}

static void choose_graphics_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
  if (gop == NULL || gop->mode == NULL || gop->query_mode == NULL || gop->set_mode == NULL) { return; }
  uint32_t best_mode = gop->mode->mode;
  uint64_t best_area = 0;
  for (UINT32 mode = 0; mode < gop->mode->max_mode; ++mode) {
    UINTN info_size = 0;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
    EFI_STATUS status = gop->query_mode(gop, mode, &info_size, &info);
    if (EFI_ERROR(status) || framebuffer_format_from_info(info) == SPORE_FB_FORMAT_NONE) { continue; }
    uint64_t area = (uint64_t)info->horizontal_resolution * (uint64_t)info->vertical_resolution;
    if (area > best_area) {
      best_area = area;
      best_mode = mode;
    }
  }
  if (best_mode != gop->mode->mode) { (void)gop->set_mode(gop, best_mode); }
}

static void capture_framebuffer(struct spore_boot_info *boot) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
  EFI_STATUS status = bs->locate_protocol((EFI_GUID *)&EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, NULL, (void **)&gop);
  if (EFI_ERROR(status) || gop == NULL) { return; }
  choose_graphics_mode(gop);
  if (gop->mode == NULL || gop->mode->info == NULL) { return; }
  uint32_t format = framebuffer_format_from_info(gop->mode->info);
  if (format == SPORE_FB_FORMAT_NONE) { return; }
  boot->framebuffer_phys = gop->mode->frame_buffer_base;
  boot->framebuffer_size = gop->mode->frame_buffer_size;
  boot->framebuffer_width = gop->mode->info->horizontal_resolution;
  boot->framebuffer_height = gop->mode->info->vertical_resolution;
  boot->framebuffer_pixels_per_scanline = gop->mode->info->pixels_per_scan_line;
  boot->framebuffer_format = format;
}

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
  struct spore_cpu_entry *cpu_entries = NULL;
  struct spore_memmap_entry *memmap = NULL;
  EFI_MEMORY_DESCRIPTOR *efi_map = NULL;
  struct spore_boot_info *boot = NULL;
  if (EFI_ERROR(alloc_pages(pages_for(sizeof(struct spore_boot_module) * MAX_MODULES), (void **)&modules)) ||
      EFI_ERROR(alloc_pages(pages_for(sizeof(struct spore_cpu_entry) * SPORE_BOOT_CPU_MAX), (void **)&cpu_entries)) ||
      EFI_ERROR(alloc_pages(pages_for(sizeof(struct spore_memmap_entry) * MAX_MEMMAP), (void **)&memmap)) ||
      EFI_ERROR(alloc_pages(16, (void **)&efi_map)) || EFI_ERROR(alloc_pages(1, (void **)&boot))) {
    uefi_puts(u"spore-boot: metadata alloc failed\r\n");
    return EFI_LOAD_ERROR;
  }
  memset(boot, 0, sizeof(*boot));
  capture_framebuffer(boot);

  uint32_t module_count = 0;
  status = load_modules(modules, &module_count);
  if (EFI_ERROR(status)) {
    uefi_puts(u"spore-boot: module load failed\r\n");
    return status;
  }

  uint32_t cpu_count = discover_cpu_topology(cpu_entries, SPORE_BOOT_CPU_MAX);
  if (cpu_count == 0) {
    uefi_puts(u"spore-boot: cpu topology failed\r\n");
    return EFI_LOAD_ERROR;
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
  boot->cpu_count = cpu_count;
  boot->modules_phys = (uint64_t)(uintptr_t)modules;
  boot->cpu_entries_phys = (uint64_t)(uintptr_t)cpu_entries;
  boot->hhdm_offset = HHDM_OFFSET;
  boot->kernel_phys_base = kernel_phys_base;
  boot->kernel_virt_base = kernel_virt_base;
  boot->uart_phys = PL011_PHYS;
  boot->realtime_epoch_sec = read_realtime_epoch();

  status = final_memory_map(memmap, &boot->memmap_count, efi_map, 16 * PAGE_SIZE, image);
  if (EFI_ERROR(status)) { return status; }

  uart_puts("spore-boot: exited boot services\n");
  uint64_t mair;
  uint64_t tcr;
  mmu_regs(&mair, &tcr);
  const struct spore_boot_info *boot_hhdm =
    (const struct spore_boot_info *)(uintptr_t)(HHDM_OFFSET + (uint64_t)(uintptr_t)boot);

  __asm__ volatile("msr daifset, #0xf\n"
                   "msr mair_el1, %[mair]\n"
                   "msr tcr_el1, %[tcr]\n"
                   "msr ttbr0_el1, %[root]\n"
                   "msr ttbr1_el1, %[root]\n"
                   "dsb ishst\n"
                   "tlbi vmalle1\n"
                   "dsb ish\n"
                   "isb\n"
                   "mov x0, %[boot]\n"
                   "br %[entry]\n"
                   :
                   : [mair] "r"(mair), [tcr] "r"(tcr), [root] "r"(ttbr1), [entry] "r"(entry), [boot] "r"(boot_hhdm)
                   : "x0", "memory");
  __builtin_unreachable();
}
