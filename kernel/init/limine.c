#include "limine.h"
#include "init/boot_info.h"
#include "lib/kstr.h"
#include "lib/mem.h"

#if defined(__riscv)
constexpr uint64_t PAGING_MODE = LIMINE_PAGING_MODE_RISCV_SV48;
#elif defined(__x86_64__)
constexpr uint64_t PAGING_MODE = LIMINE_PAGING_MODE_X86_64_4LVL;
#else
constexpr uint64_t PAGING_MODE = LIMINE_PAGING_MODE_AARCH64_4LVL;
#endif

#define REQUEST __attribute__((used, section(".limine_requests")))
__attribute__((used, section(".limine_requests_start"))) static volatile uint64_t requests_start[] =
  LIMINE_REQUESTS_START_MARKER;
REQUEST static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(6);
REQUEST static volatile struct limine_hhdm_request hhdm = {.id = LIMINE_HHDM_REQUEST_ID};
REQUEST static volatile struct limine_memmap_request memory = {.id = LIMINE_MEMMAP_REQUEST_ID};
REQUEST static volatile struct limine_mp_request mp = {.id = LIMINE_MP_REQUEST_ID};
REQUEST static volatile struct limine_module_request modules = {.id = LIMINE_MODULE_REQUEST_ID};
REQUEST static volatile struct limine_date_at_boot_request date = {.id = LIMINE_DATE_AT_BOOT_REQUEST_ID};
REQUEST static volatile struct limine_paging_mode_request paging = {
  .id = LIMINE_PAGING_MODE_REQUEST_ID,
  .mode = PAGING_MODE,
  .max_mode = PAGING_MODE,
  .min_mode = PAGING_MODE,
};
__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t requests_end[] =
  LIMINE_REQUESTS_END_MARKER;

const struct spore_boot_info *limine_boot_info(void) {
  static struct spore_boot_info boot;
  static struct spore_memmap_entry usable[256];
  static struct spore_boot_module files[64];
  if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision) || !hhdm.response || !memory.response || !mp.response ||
      mp.response->cpu_count == 0 || !paging.response || paging.response->mode != PAGING_MODE) {
    return NULL;
  }

  boot.hhdm_offset = hhdm.response->offset;
  boot.mp = mp.response;
  boot.memmap = usable;
  for (uint64_t i = 0; i < memory.response->entry_count; ++i) {
    const struct limine_memmap_entry *e = memory.response->entries[i];
    if (e->type != LIMINE_MEMMAP_USABLE) { continue; }
    if (boot.memmap_count == sizeof(usable) / sizeof(usable[0])) { return NULL; }
    usable[boot.memmap_count++] = (struct spore_memmap_entry){
      .base = e->base,
      .length = e->length,
      .type = SPORE_MEMMAP_USABLE,
    };
  }
  if (boot.memmap_count == 0) { return NULL; }
  if (date.response && date.response->timestamp > 0) { boot.realtime_epoch_sec = (uint64_t)date.response->timestamp; }

  boot.modules = files;
  if (modules.response) {
    if (modules.response->module_count > sizeof(files) / sizeof(files[0])) { return NULL; }
    for (uint64_t i = 0; i < modules.response->module_count; ++i) {
      const struct limine_file *file = modules.response->modules[i];
      const char *path = file->string && file->string[0] ? file->string : file->path;
      if (!path || path[0] != '/' || kstrlen(path) >= sizeof(files[i].path)) { return NULL; }
      files[i].phys_addr = (uintptr_t)file->address - boot.hhdm_offset;
      files[i].size = file->size;
      copy_cstr(files[i].path, sizeof(files[i].path), path);
      ++boot.module_count;
    }
  }
  return &boot;
}
