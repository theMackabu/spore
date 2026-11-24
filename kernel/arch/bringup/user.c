#include "user.h"
#include "init/event.h"
#include "platform.h"
#include "exec/elf.h"
#include "exec/elf_rust.h"
#include "lib/kstr.h"
#include "lib/mem.h"

bool user_freebsd;
static uint64_t *root;
static uint64_t *pte(uint64_t va, bool create) {
  uint64_t *table = root;
  for (unsigned level = page_levels() - 1; level; --level) {
    uint64_t *slot = &table[(va >> (12 + level * 9)) & 511];
    if (!*slot) {
      if (!create) { return NULL; }
      uint64_t pa = pmm_alloc_zero_page();
      if (!pa) { return NULL; }
      *slot = table_entry(pa);
    }
    table = pmm_phys_to_virt(entry_phys(*slot));
  }
  return &table[(va >> 12) & 511];
}

bool map_page(uint64_t va, uint64_t pa, unsigned flags) {
  uint64_t *slot = pte(va, true);
  if (!slot || *slot || !pa) { return false; }
  *slot = page_entry(pa, flags);
  return true;
}

static void *user_pointer(uint64_t va, unsigned flags) {
  if (va >= USER_TOP) { return NULL; }
  uint64_t *slot = pte(va, false);
  if (!slot || !page_allows(*slot, flags | PAGE_USER)) { return NULL; }
  return pmm_phys_to_virt(entry_phys(*slot) + (va & (PAGE_SIZE - 1)));
}

struct image { const uint8_t *data; size_t size; };
static bool image_read(void *ctx, uint64_t off, void *dst, size_t n) {
  const struct image *img = ctx;
  if (off > img->size || n > img->size - off) { return false; }
  kmemcpy(dst, img->data + off, n);
  return true;
}

static bool load_segment(void *ctx, const struct spore_elf_program *p, uint64_t addr, uint64_t start, uint64_t end) {
  if (start < PAGE_SIZE || end > USER_TOP - 65536 || (p->flags & 3) == 3) { return false; }
  unsigned flags = PAGE_USER | PAGE_READ;
  if (p->flags & 1) { flags |= PAGE_EXEC; }
  if (p->flags & 2) { flags |= PAGE_WRITE; }
  for (uint64_t va = start; va < end; va += PAGE_SIZE) {
    uint64_t pa = pmm_alloc_zero_page();
    if (!map_page(va, pa, flags)) { return false; }
    uint64_t from = va < addr ? addr : va;
    uint64_t to = va + PAGE_SIZE;
    if (to > addr + p->filesz) { to = addr + p->filesz; }
    if (from < to && !image_read(ctx, p->offset + (from - addr), pmm_phys_to_virt(pa + from - va), to - from)) {
      return false;
    }
  }
  return true;
}

[[noreturn]] void user_finish(char kind, unsigned value) {
  app_event(status_write, kind, value);
  platform_poweroff();
}

[[noreturn]] void user_start(const struct spore_boot_info *boot) {
  uint64_t pa = pmm_alloc_zero_page();
  if (!pa) { platform_poweroff(); }
  root = pmm_phys_to_virt(pa);
  const uint64_t *firmware = pmm_phys_to_virt(active_root());
  kmemcpy(root + 256, firmware + 256, PAGE_SIZE / 2);
  install_root(pa);
  if (!status_init(boot->hhdm_offset)) { platform_poweroff(); }
  const struct spore_boot_module *module = &boot->modules[0];
  struct image img = {pmm_phys_to_virt(module->phys_addr), module->size};
  struct loaded_elf elf;
  char interp[2];
  if (boot->module_count != 2 || !str_eq(module->path, "/app") ||
      !spore_elf_load(image_read, &img, img.size, 0, load_segment, &img, &elf, interp, sizeof(interp)) ||
      interp[0] || !user_pointer(elf.entry, PAGE_EXEC)) { user_finish('B', 1); }

  for (uint64_t va = USER_TOP - 65536; va < USER_TOP; va += PAGE_SIZE) {
    if (!map_page(va, pmm_alloc_zero_page(), PAGE_USER | PAGE_READ | PAGE_WRITE)) { user_finish('B', 2); }
  }
  const struct spore_boot_module *args = &boot->modules[1];
  if (!str_eq(args->path, "/argv") || !args->size || args->size > 32768) { user_finish('B', 3); }
  const uint8_t *bytes = pmm_phys_to_virt(args->phys_addr);
  if (bytes[args->size - 1]) { user_finish('B', 3); }
  uint64_t strings = USER_TOP - args->size;
  size_t argc = 0;
  for (size_t i = 0; i < args->size; ++i) {
    *(uint8_t *)user_pointer(strings + i, PAGE_WRITE) = bytes[i];
    if (!bytes[i]) { ++argc; }
  }
  uint64_t aux[] = {0, 0, 3, elf.phdr, 4, elf.phent, 5, elf.phnum, 6, PAGE_SIZE, 9, elf.entry, 0, 0};
  if (argc > 256) { user_finish('B', 3); }
  uint64_t sp = (strings - sizeof(aux) - (argc + 1) * 8) & ~15ull;
  uint64_t cursor = sp;
  *(uint64_t *)user_pointer(cursor, PAGE_WRITE) = argc;
  cursor += 8;
  for (size_t i = 0; i < args->size; ++i) {
    if (!i || !bytes[i - 1]) {
      *(uint64_t *)user_pointer(cursor, PAGE_WRITE) = strings + i;
      cursor += 8;
    }
  }
  for (size_t i = 0; i < sizeof(aux) / sizeof(*aux); ++i, cursor += 8) {
    *(uint64_t *)user_pointer(cursor, PAGE_WRITE) = aux[i];
  }
  user_freebsd = elf.osabi == 9;
  app_event(status_write, 'U', 0);
  user_enter(elf.entry, sp);
}

int64_t user_write(uint64_t fd, uint64_t va, uint64_t n) {
  if (fd != 1 && fd != 2) { return -9; }
  if (n && (va >= USER_TOP || n > USER_TOP - va)) { return -14; }
  for (uint64_t i = 0; i < n;) {
    const uint8_t *p = user_pointer(va + i, PAGE_READ);
    if (!p) { return i ? (int64_t)i : -14; }
    size_t count = PAGE_SIZE - ((va + i) & (PAGE_SIZE - 1));
    if (count > n - i) { count = n - i; }
    for (size_t j = 0; j < count; ++j) { console_putc(p[j]); }
    i += count;
  }
  return n;
}
