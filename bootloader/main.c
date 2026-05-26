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
#define ELF_MAGIC 0x464c457f
#define EM_AARCH64 183
#define PT_LOAD 1

struct loaded_file {
  void *data;
  uint64_t size;
  uint64_t phys;
};

struct elf64_ehdr {
  uint8_t e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
};

struct elf64_phdr {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
};

static EFI_SYSTEM_TABLE *st;
static EFI_BOOT_SERVICES *bs;
static EFI_FILE_PROTOCOL *root;
static uint64_t (*page_table_pool)[PT_ENTRIES];
static uint32_t page_table_pool_used;

static void *memset(void *dst, int c, uint64_t n) {
  uint8_t *d = dst;
  for (uint64_t i = 0; i < n; ++i) {
    d[i] = (uint8_t)c;
  }
  return dst;
}

static void *memcpy(void *dst, const void *src, uint64_t n) {
  uint8_t *d = dst;
  const uint8_t *s = src;
  for (uint64_t i = 0; i < n; ++i) {
    d[i] = s[i];
  }
  return dst;
}

static uint64_t strlen8(const char *s) {
  uint64_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}

static void uefi_puts(const CHAR16 *s) {
  st->con_out->output_string(st->con_out, (CHAR16 *)s);
}

static void set_boot_timeout_zero(void) {
  if (st->runtime_services == NULL || st->runtime_services->set_variable == NULL) { return; }
  UINT16 timeout = 0;
  (void)st->runtime_services->set_variable(u"Timeout", (EFI_GUID *)&EFI_GLOBAL_VARIABLE_GUID,
                                           EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                             EFI_VARIABLE_RUNTIME_ACCESS,
                                           sizeof(timeout), &timeout);
}

static int is_leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint64_t unix_days_before_year(int year) {
  uint64_t days = 0;
  for (int y = 1970; y < year; ++y) {
    days += is_leap_year(y) ? 366u : 365u;
  }
  return days;
}

static uint64_t unix_epoch_from_efi_time(const EFI_TIME *time) {
  static const uint8_t month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (time == NULL || time->year < 1970 || time->month < 1 || time->month > 12 || time->day < 1 || time->day > 31) {
    return 0;
  }
  uint64_t days = unix_days_before_year(time->year);
  for (uint8_t m = 1; m < time->month; ++m) {
    days += month_days[m - 1];
    if (m == 2 && is_leap_year(time->year)) { ++days; }
  }
  days += time->day - 1;
  int64_t seconds = (int64_t)(days * 86400ull + (uint64_t)time->hour * 3600ull + (uint64_t)time->minute * 60ull +
                              (uint64_t)time->second);
  if (time->timezone >= -1440 && time->timezone <= 1440) { seconds -= (int64_t)time->timezone * 60; }
  return seconds > 0 ? (uint64_t)seconds : 0;
}

static uint64_t read_realtime_epoch(void) {
  if (st->runtime_services == NULL || st->runtime_services->get_time == NULL) { return 0; }
  EFI_TIME time;
  if (EFI_ERROR(st->runtime_services->get_time(&time, NULL))) { return 0; }
  return unix_epoch_from_efi_time(&time);
}

static void uart_putc(char c) {
  volatile uint32_t *uart = (volatile uint32_t *)(uintptr_t)PL011_PHYS;
  volatile uint32_t *fr = (volatile uint32_t *)(uintptr_t)(PL011_PHYS + 0x18);
  while ((*fr & (1u << 5)) != 0) {}
  if (c == '\n') { uart_putc('\r'); }
  *uart = (uint32_t)c;
}

static void uart_puts(const char *s) {
  while (*s != '\0') {
    uart_putc(*s++);
  }
}

static uint64_t align_down(uint64_t value, uint64_t align) {
  return value & ~(align - 1);
}

static uint64_t align_up(uint64_t value, uint64_t align) {
  return (value + align - 1) & ~(align - 1);
}

static uint64_t pages_for(uint64_t size) {
  return align_up(size, PAGE_SIZE) / PAGE_SIZE;
}

static EFI_STATUS alloc_pages(uint64_t pages, void **out) {
  EFI_PHYSICAL_ADDRESS pa = 0;
  EFI_STATUS status = bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, pages, &pa);
  if (EFI_ERROR(status)) { return status; }
  *out = (void *)(uintptr_t)pa;
  memset(*out, 0, pages * PAGE_SIZE);
  return EFI_SUCCESS;
}

static EFI_STATUS open_root(EFI_HANDLE image) {
  EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
  EFI_STATUS status = bs->handle_protocol(image, (EFI_GUID *)&EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&loaded);
  if (EFI_ERROR(status)) { return status; }
  status = bs->handle_protocol(loaded->device_handle, (EFI_GUID *)&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void **)&fs);
  if (EFI_ERROR(status)) { return status; }
  return fs->open_volume(fs, &root);
}

static EFI_STATUS file_size(EFI_FILE_PROTOCOL *file, uint64_t *size) {
  uint8_t info_buf[512];
  UINTN info_size = sizeof(info_buf);
  EFI_STATUS status = file->get_info(file, (EFI_GUID *)&EFI_FILE_INFO_GUID, &info_size, info_buf);
  if (EFI_ERROR(status)) { return status; }
  EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
  *size = info->file_size;
  return EFI_SUCCESS;
}

static EFI_STATUS read_file(CHAR16 *path, struct loaded_file *out) {
  EFI_FILE_PROTOCOL *file = NULL;
  EFI_STATUS status = root->open(root, &file, path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(status)) { return status; }
  uint64_t size = 0;
  status = file_size(file, &size);
  if (EFI_ERROR(status)) {
    file->close(file);
    return status;
  }
  void *data = NULL;
  status = alloc_pages(pages_for(size == 0 ? 1 : size), &data);
  if (EFI_ERROR(status)) {
    file->close(file);
    return status;
  }
  UINTN read_size = (UINTN)size;
  status = file->read(file, &read_size, data);
  file->close(file);
  if (EFI_ERROR(status) || read_size != size) { return EFI_LOAD_ERROR; }
  out->data = data;
  out->size = size;
  out->phys = (uint64_t)(uintptr_t)data;
  return EFI_SUCCESS;
}

static void ascii_to_efi_path(const char *src, CHAR16 *dst, uint64_t cap) {
  uint64_t j = 0;
  for (uint64_t i = 0; src[i] != '\0' && j + 1 < cap; ++i) {
    char c = src[i] == '/' ? '\\' : src[i];
    dst[j++] = (CHAR16)c;
  }
  dst[j] = 0;
}

static int next_line(char **cursor, char *line, uint64_t cap) {
  char *p = *cursor;
  if (*p == '\0') { return 0; }
  uint64_t n = 0;
  while (*p != '\0' && *p != '\n' && *p != '\r') {
    if (n + 1 < cap) { line[n++] = *p; }
    ++p;
  }
  while (*p == '\n' || *p == '\r') {
    ++p;
  }
  line[n] = '\0';
  *cursor = p;
  return 1;
}

static int split_manifest_line(char *line, char **esp_path, char **target_path) {
  char *p = line;
  while (*p == ' ' || *p == '\t') {
    ++p;
  }
  if (*p == '\0' || *p == '#') { return 0; }
  *esp_path = p;
  while (*p != '\0' && *p != ' ' && *p != '\t') {
    ++p;
  }
  if (*p == '\0') { return -1; }
  *p++ = '\0';
  while (*p == ' ' || *p == '\t') {
    ++p;
  }
  if (*p == '\0') { return -1; }
  *target_path = p;
  while (*p != '\0' && *p != ' ' && *p != '\t') {
    ++p;
  }
  *p = '\0';
  return 1;
}

static EFI_STATUS load_modules(struct spore_boot_module *modules, uint32_t *count) {
  struct loaded_file manifest;
  EFI_STATUS status = read_file(u"\\boot\\modules.txt", &manifest);
  if (EFI_ERROR(status)) { return status; }
  char *cursor = manifest.data;
  char line[256];
  uint32_t n = 0;
  while (next_line(&cursor, line, sizeof(line))) {
    char *esp_path = NULL;
    char *target_path = NULL;
    int split = split_manifest_line(line, &esp_path, &target_path);
    if (split == 0) { continue; }
    if (split < 0 || n >= MAX_MODULES) { return EFI_LOAD_ERROR; }
    CHAR16 efi_path[160];
    ascii_to_efi_path(esp_path, efi_path, sizeof(efi_path) / sizeof(efi_path[0]));
    struct loaded_file file;
    status = read_file(efi_path, &file);
    if (EFI_ERROR(status)) { return status; }
    modules[n].phys_addr = file.phys;
    modules[n].size = file.size;
    uint64_t len = strlen8(target_path);
    if (len >= SPORE_BOOT_MODULE_PATH_MAX) { return EFI_LOAD_ERROR; }
    for (uint64_t i = 0; i <= len; ++i) {
      modules[n].path[i] = target_path[i];
    }
    ++n;
  }
  *count = n;
  return EFI_SUCCESS;
}

static EFI_STATUS load_kernel(struct loaded_file *kernel_file, uint64_t *kernel_phys_base, uint64_t *kernel_virt_base,
                              uint64_t *kernel_span, uint64_t *entry) {
  EFI_STATUS status = read_file(u"\\boot\\kernel.elf", kernel_file);
  if (EFI_ERROR(status)) { return status; }
  if (kernel_file->size < sizeof(struct elf64_ehdr)) { return EFI_LOAD_ERROR; }
  struct elf64_ehdr *eh = kernel_file->data;
  if (*(uint32_t *)eh->e_ident != ELF_MAGIC || eh->e_machine != EM_AARCH64 ||
      eh->e_phoff + (uint64_t)eh->e_phentsize * eh->e_phnum > kernel_file->size) {
    return EFI_LOAD_ERROR;
  }

  uint64_t min_vaddr = UINT64_MAX;
  uint64_t max_vaddr = 0;
  for (uint16_t i = 0; i < eh->e_phnum; ++i) {
    struct elf64_phdr *ph =
      (struct elf64_phdr *)((uint8_t *)kernel_file->data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
    if (ph->p_type != PT_LOAD) { continue; }
    uint64_t start = align_down(ph->p_vaddr, PAGE_SIZE);
    uint64_t end = align_up(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
    if (start < min_vaddr) { min_vaddr = start; }
    if (end > max_vaddr) { max_vaddr = end; }
  }
  if (min_vaddr == UINT64_MAX || max_vaddr <= min_vaddr) { return EFI_LOAD_ERROR; }

  void *load = NULL;
  status = alloc_pages(pages_for(max_vaddr - min_vaddr), &load);
  if (EFI_ERROR(status)) { return status; }
  for (uint16_t i = 0; i < eh->e_phnum; ++i) {
    struct elf64_phdr *ph =
      (struct elf64_phdr *)((uint8_t *)kernel_file->data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
    if (ph->p_type != PT_LOAD) { continue; }
    if (ph->p_offset + ph->p_filesz > kernel_file->size || ph->p_filesz > ph->p_memsz) { return EFI_LOAD_ERROR; }
    uint64_t dst_off = ph->p_vaddr - min_vaddr;
    memcpy((uint8_t *)load + dst_off, (uint8_t *)kernel_file->data + ph->p_offset, ph->p_filesz);
    memset((uint8_t *)load + dst_off + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
  }
  *kernel_phys_base = (uint64_t)(uintptr_t)load;
  *kernel_virt_base = min_vaddr;
  *kernel_span = max_vaddr - min_vaddr;
  *entry = eh->e_entry;
  return EFI_SUCCESS;
}

static uint64_t *new_page_table(void) {
  if (page_table_pool == NULL) { return NULL; }
  if (page_table_pool_used >= PAGE_TABLE_POOL_PAGES) {
    uefi_puts(u"spore-boot: page table pool exhausted\r\n");
    return NULL;
  }
  uint64_t *page = page_table_pool[page_table_pool_used++];
  memset(page, 0, PAGE_SIZE);
  return page;
}

static uint64_t pt_index(uint64_t va, uint32_t shift) {
  return (va >> shift) & 0x1ffu;
}

static uint64_t table_desc(uint64_t *table) {
  return (uint64_t)(uintptr_t)table | 0x3ull;
}

static uint64_t *ensure_table(uint64_t *table, uint64_t index) {
  if ((table[index] & 1u) == 0) {
    uint64_t *child = new_page_table();
    if (child == NULL) { return NULL; }
    table[index] = table_desc(child);
  }
  return (uint64_t *)(uintptr_t)(table[index] & 0x0000fffffffff000ull);
}

static int map_page(uint64_t *root_table, uint64_t va, uint64_t pa, uint64_t attrs) {
  uint64_t *l1 = ensure_table(root_table, pt_index(va, 39));
  if (l1 == NULL) { return 0; }
  uint64_t *l2 = ensure_table(l1, pt_index(va, 30));
  if (l2 == NULL) { return 0; }
  uint64_t *l3 = ensure_table(l2, pt_index(va, 21));
  if (l3 == NULL) { return 0; }
  l3[pt_index(va, 12)] = (pa & 0x0000fffffffff000ull) | attrs | 0x3ull;
  return 1;
}

static int map_l2_block(uint64_t *root_table, uint64_t va, uint64_t pa, uint64_t attrs) {
  uint64_t *l1 = ensure_table(root_table, pt_index(va, 39));
  if (l1 == NULL) { return 0; }
  uint64_t *l2 = ensure_table(l1, pt_index(va, 30));
  if (l2 == NULL) { return 0; }
  l2[pt_index(va, 21)] = (pa & 0x0000ffffffe00000ull) | attrs | 0x1ull;
  return 1;
}

static int map_l1_block(uint64_t *root_table, uint64_t va, uint64_t pa, uint64_t attrs) {
  uint64_t *l1 = ensure_table(root_table, pt_index(va, 39));
  if (l1 == NULL) { return 0; }
  l1[pt_index(va, 30)] = (pa & 0x0000ffffc0000000ull) | attrs | 0x1ull;
  return 1;
}

static uint64_t current_el(void) {
  uint64_t el;
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
  return (el >> 2) & 3u;
}

static void install_ttbr1(uint64_t root_pa) {
  uint64_t mair = 0xffull | (0x00ull << 16);
  uint64_t tcr;
  __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
  uint64_t clear = (0x3full << 16) | (1ull << 23) | (3ull << 24) | (3ull << 26) | (3ull << 28) | (3ull << 30);
  tcr &= ~clear;
  tcr |= (16ull << 16) | (1ull << 24) | (1ull << 26) | (3ull << 28) | (2ull << 30);
  __asm__ volatile("msr mair_el1, %0\n"
                   "msr tcr_el1, %1\n"
                   "msr ttbr1_el1, %2\n"
                   "dsb ishst\n"
                   "tlbi vmalle1\n"
                   "dsb ish\n"
                   "isb\n"
                   :
                   : "r"(mair), "r"(tcr), "r"(root_pa)
                   : "memory");
}

static int build_page_tables(uint64_t kernel_phys_base, uint64_t kernel_virt_base, uint64_t kernel_span, uint64_t entry,
                             uint64_t *ttbr1_out) {
  (void)entry;
  uint64_t *root_table = new_page_table();
  if (root_table == NULL) {
    uefi_puts(u"spore-boot: root table alloc failed\r\n");
    return 0;
  }
  const uint64_t attr_normal = 0ull << 2;
  const uint64_t af = 1ull << 10;
  const uint64_t sh_inner = 3ull << 8;
  const uint64_t pxn = 1ull << 53;
  const uint64_t uxn = 1ull << 54;
  const uint64_t normal_rw_nx = attr_normal | af | sh_inner | pxn | uxn;
  const uint64_t kernel_rx = attr_normal | af | sh_inner | uxn;
  const uint64_t kernel_rw_nx = attr_normal | af | sh_inner | pxn | uxn;

  uint64_t *hhdm_l1 = new_page_table();
  if (hhdm_l1 == NULL) {
    uefi_puts(u"spore-boot: hhdm l1 alloc failed\r\n");
    return 0;
  }
  root_table[pt_index(HHDM_OFFSET, 39)] = table_desc(hhdm_l1);
  for (uint64_t gb = 0; gb < HHDM_SIZE; gb += 0x40000000ull) {
    uint64_t *l2 = new_page_table();
    if (l2 == NULL) {
      uefi_puts(u"spore-boot: hhdm map failed\r\n");
      return 0;
    }
    hhdm_l1[pt_index(HHDM_OFFSET + gb, 30)] = table_desc(l2);
    for (uint64_t off = 0; off < 0x40000000ull; off += 0x200000ull) {
      uint64_t pa = gb + off;
      l2[pt_index(HHDM_OFFSET + pa, 21)] = (pa & 0x0000ffffffe00000ull) | normal_rw_nx | 0x1ull;
    }
  }

  uint64_t kernel_map = align_up(kernel_span, PAGE_SIZE);
  for (uint64_t off = 0; off < kernel_map; off += PAGE_SIZE) {
    uint64_t attrs = off < 0x100000ull ? kernel_rx : kernel_rw_nx;
    if (!map_page(root_table, kernel_virt_base + off, kernel_phys_base + off, attrs)) {
      uefi_puts(u"spore-boot: kernel map failed\r\n");
      return 0;
    }
  }
  *ttbr1_out = (uint64_t)(uintptr_t)root_table;
  return 1;
}

static uint32_t efi_mem_type(uint32_t efi_type) {
  switch (efi_type) {
  case 7:
    return SPORE_MEMMAP_USABLE;
  case 9:
    return SPORE_MEMMAP_ACPI_RECLAIMABLE;
  case 10:
    return SPORE_MEMMAP_ACPI_NVS;
  case 11:
  case 12:
  case 13:
    return SPORE_MEMMAP_MMIO;
  case 3:
  case 4:
    return SPORE_MEMMAP_BOOTLOADER_RECLAIMABLE;
  default:
    return SPORE_MEMMAP_RESERVED;
  }
}

static EFI_STATUS final_memory_map(struct spore_memmap_entry *out, uint32_t *out_count, EFI_MEMORY_DESCRIPTOR *efi_map,
                                   UINTN efi_map_capacity, EFI_HANDLE image) {
  for (;;) {
    UINTN map_size = efi_map_capacity;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    EFI_STATUS status = bs->get_memory_map(&map_size, efi_map, &map_key, &desc_size, &desc_version);
    if (status == EFI_BUFFER_TOO_SMALL) { return status; }
    if (EFI_ERROR(status)) { return status; }
    uint32_t count = (uint32_t)(map_size / desc_size);
    if (count > MAX_MEMMAP) { return EFI_LOAD_ERROR; }
    for (uint32_t i = 0; i < count; ++i) {
      EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)efi_map + (uint64_t)i * desc_size);
      out[i].base = desc->physical_start;
      out[i].length = desc->number_of_pages * PAGE_SIZE;
      out[i].type = efi_mem_type(desc->type);
      out[i].reserved = 0;
    }
    *out_count = count;
    status = bs->exit_boot_services(image, map_key);
    if (status == EFI_INVALID_PARAMETER) { continue; }
    return status;
  }
}

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
      EFI_ERROR(alloc_pages(16, (void **)&efi_map)) || EFI_ERROR(alloc_pages(1, (void **)&boot)) ||
      EFI_ERROR(alloc_pages(PAGE_TABLE_POOL_PAGES, (void **)&page_table_pool))) {
    uefi_puts(u"spore-boot: metadata alloc failed\r\n");
    return EFI_LOAD_ERROR;
  }
  page_table_pool_used = 0;

  uint32_t module_count = 0;
  status = load_modules(modules, &module_count);
  if (EFI_ERROR(status)) {
    uefi_puts(u"spore-boot: module load failed\r\n");
    return status;
  }

  uint64_t ttbr1 = 0;
  if (!build_page_tables(kernel_phys_base, kernel_virt_base, kernel_span, entry, &ttbr1)) {
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
