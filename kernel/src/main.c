#include "arch/aarch64/exceptions.h"
#include "arch/aarch64/timer.h"
#include "boot_info.h"
#include "cell.h"
#include "elf/loader.h"
#include "exec/stack.h"
#include "ext2.h"
#include "kprintf.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "net.h"
#include "pl011.h"
#include "ramfs.h"
#include "random.h"
#include "vfs.h"
#include "virtio_blk.h"
#include "virtio_console.h"
#include "virtio_net.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const struct spore_boot_info *boot;
static char kernel_log[16384];
static uint64_t kernel_log_head;
static uint64_t kernel_log_len;
static bool kernel_log_handoff;

static void klog_store(char c) {
  kernel_log[kernel_log_head] = c;
  kernel_log_head = (kernel_log_head + 1u) % sizeof(kernel_log);
  if (kernel_log_len < sizeof(kernel_log)) { ++kernel_log_len; }
}

uint64_t klog_copy(char *dst, uint64_t cap) {
  uint64_t n = kernel_log_len < cap ? kernel_log_len : cap;
  uint64_t start = (kernel_log_head + sizeof(kernel_log) - kernel_log_len) % sizeof(kernel_log);
  for (uint64_t i = 0; i < n; ++i) {
    dst[i] = kernel_log[(start + i) % sizeof(kernel_log)];
  }
  return n;
}

void kputc(char c) {
  klog_store(c);
  if (virtio_console_ready()) {
    virtio_console_putc(c);
  } else if (!kernel_log_handoff) {
    pl011_putc(c);
  }
}

void kputs(const char *s) {
  while (*s != '\0') {
    kputc(*s++);
  }
}

static void print_unsigned(uint64_t value, uint64_t base) {
  char buf[32];
  size_t i = 0;

  if (value == 0) {
    kputc('0');
    return;
  }

  while (value != 0) {
    uint64_t digit = value % base;
    buf[i++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
    value /= base;
  }
  while (i > 0) {
    kputc(buf[--i]);
  }
}

static void print_signed(int64_t value) {
  if (value < 0) {
    kputc('-');
    print_unsigned((uint64_t)-value, 10);
    return;
  }
  print_unsigned((uint64_t)value, 10);
}

void kprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  for (const char *p = fmt; *p != '\0'; ++p) {
    if (*p != '%') {
      kputc(*p);
      continue;
    }

    char spec = *++p;
    switch (spec) {
    case '%':
      kputc('%');
      break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      kputs(s == NULL ? "(null)" : s);
      break;
    }
    case 'd':
      print_signed((int64_t)va_arg(ap, int));
      break;
    case 'u':
      print_unsigned((uint64_t)va_arg(ap, unsigned int), 10);
      break;
    case 'x':
      print_unsigned((uint64_t)va_arg(ap, unsigned int), 16);
      break;
    case 'p':
      kputs("0x");
      print_unsigned((uint64_t)(uintptr_t)va_arg(ap, void *), 16);
      break;
    default:
      kputc('%');
      kputc(spec);
      break;
    }
  }

  va_end(ap);
}

static uint64_t current_el(void) {
  uint64_t el;
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
  return (el >> 2) & 0x3;
}

enum {
  PL011_PHYS = 0x09000000,
  VIRTIO_MMIO_PHYS = 0x0a000000,
  VIRTIO_MMIO_PAGES = 4,
  GICD_PHYS = 0x08000000,
  GICR_PHYS = 0x080a0000,
  GICR_SGI_PHYS = 0x080b0000,
  EARLY_PAGE_SIZE = 0x1000,
  PT_ENTRIES = 512,
  EARLY_L3_TABLES = 8,
};

static uint64_t early_l1[PT_ENTRIES] __attribute__((aligned(EARLY_PAGE_SIZE)));
static uint64_t early_l2[PT_ENTRIES] __attribute__((aligned(EARLY_PAGE_SIZE)));
static uint64_t early_l3[EARLY_L3_TABLES][PT_ENTRIES] __attribute__((aligned(EARLY_PAGE_SIZE)));
static size_t early_l3_used;
static uint8_t kernel_stack[64 * 1024] __attribute__((aligned(16)));
static struct ramfs boot_ramfs;
static struct ext2_fs root_ext2;
static bool root_ext2_ready;

static bool ext2_blk_read(void *ctx, uint64_t offset, void *dst, uint32_t len) {
  (void)ctx;
  return virtio_blk_read(offset, dst, len);
}

static bool ext2_blk_write(void *ctx, uint64_t offset, const void *src, uint32_t len) {
  (void)ctx;
  return virtio_blk_write(offset, src, len);
}

static bool vfs_elf_read_at(void *ctx, uint64_t offset, void *dst, size_t len) {
  const struct vfs_node *node = ctx;
  return node != NULL && vfs_read(node, offset, dst, len) == len;
}

static bool make_elf_reader(const struct vfs_node *node, struct elf_reader *reader) {
  if (node == NULL || reader == NULL || node->is_dir || node->device != RAMFS_DEV_NONE) { return false; }
  *reader = (struct elf_reader){
    .read_at = vfs_elf_read_at,
    .ctx = (void *)node,
    .node = node,
    .size = node->size,
  };
  return true;
}

static void probe_ext2_root(void) {
  if (!ext2_mount_rw(&root_ext2, ext2_blk_read, ext2_blk_write, NULL)) {
    kprintf("[spore] ext2: mount failed\n");
    return;
  }
  root_ext2_ready = true;

  struct ext2_node motd;
  if (ext2_lookup(&root_ext2, "/etc/motd", &motd)) {
    kprintf("[spore] ext2: /etc/motd size=%u\n", (unsigned)motd.size);
  }

  struct ext2_node bin;
  if (ext2_lookup(&root_ext2, "/bin", &bin)) {
    struct ext2_dirent ent;
    unsigned count = 0;
    while (ext2_dirent(&root_ext2, &bin, count, &ent)) {
      ++count;
    }
    kprintf("[spore] ext2: /bin entries=%u\n", count);
  }
}

static uint64_t kernel_virt_to_phys(uintptr_t va) {
  return (uint64_t)va - boot->kernel_virt_base + boot->kernel_phys_base;
}

static uint64_t *phys_to_hhdm(uint64_t pa) {
  return (uint64_t *)(uintptr_t)(boot->hhdm_offset + pa);
}

static uint64_t pt_entry_address(uint64_t entry) {
  return entry & 0x0000fffffffff000ull;
}

static uint64_t table_descriptor(uint64_t *table) {
  return kernel_virt_to_phys((uintptr_t)table) | 0x3ull;
}

static void zero_page(uint64_t *page) {
  for (size_t i = 0; i < PT_ENTRIES; ++i) {
    page[i] = 0;
  }
}

static uint64_t *alloc_early_l3(void) {
  if (early_l3_used >= EARLY_L3_TABLES) { return NULL; }
  uint64_t *table = early_l3[early_l3_used++];
  zero_page(table);
  return table;
}

static void split_l2_block(uint64_t *l2, size_t index) {
  uint64_t entry = l2[index];
  if ((entry & 0x3ull) != 0x1ull) { return; }
  uint64_t *table = alloc_early_l3();
  if (table == NULL) { return; }
  uint64_t base = entry & 0x0000ffffffe00000ull;
  uint64_t attrs = entry & ~0x0000ffffffe00000ull;
  attrs &= ~0x3ull;
  for (size_t i = 0; i < PT_ENTRIES; ++i) {
    table[i] = (base + i * EARLY_PAGE_SIZE) | attrs | 0x3ull;
  }
  l2[index] = table_descriptor(table);
}

static void map_device_page(uint64_t pa) {
  const uint64_t va = boot->hhdm_offset + pa;
  uint64_t ttbr1;
  __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));

  uint64_t *l0 = phys_to_hhdm(pt_entry_address(ttbr1));
  const size_t l0i = (va >> 39) & 0x1ff;
  const size_t l1i = (va >> 30) & 0x1ff;
  const size_t l2i = (va >> 21) & 0x1ff;
  const size_t l3i = (va >> 12) & 0x1ff;

  if ((l0[l0i] & 0x1) == 0) { l0[l0i] = table_descriptor(early_l1); }
  uint64_t *l1 = phys_to_hhdm(pt_entry_address(l0[l0i]));

  if ((l1[l1i] & 0x1) == 0) { l1[l1i] = table_descriptor(early_l2); }
  uint64_t *l2 = phys_to_hhdm(pt_entry_address(l1[l1i]));

  if ((l2[l2i] & 0x1) == 0) {
    uint64_t *table = alloc_early_l3();
    if (table == NULL) { return; }
    l2[l2i] = table_descriptor(table);
  }
  split_l2_block(l2, l2i);
  uint64_t *l3 = phys_to_hhdm(pt_entry_address(l2[l2i]));

  const uint64_t attr_index_device = 2ull << 2;
  const uint64_t ap_el1_rw = 0ull << 6;
  const uint64_t sh_inner = 3ull << 8;
  const uint64_t af = 1ull << 10;
  const uint64_t pxn = 1ull << 53;
  const uint64_t uxn = 1ull << 54;
  l3[l3i] = (pa & ~0xfffull) | attr_index_device | ap_el1_rw | sh_inner | af | pxn | uxn | 0x3ull;
}

static void map_device_pages(void) {
  zero_page(early_l1);
  zero_page(early_l2);
  for (size_t i = 0; i < EARLY_L3_TABLES; ++i) {
    zero_page(early_l3[i]);
  }
  early_l3_used = 0;

  uint64_t mair;
  __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
  mair &= ~0xffull;
  mair |= 0xffull;
  mair &= ~(0xffull << 16);
  mair |= 0x00ull << 16;
  __asm__ volatile("msr mair_el1, %0\n"
                   "isb\n"
                   :
                   : "r"(mair)
                   : "memory");

  map_device_page(PL011_PHYS);
  for (size_t i = 0; i < VIRTIO_MMIO_PAGES; ++i) {
    map_device_page(VIRTIO_MMIO_PHYS + i * EARLY_PAGE_SIZE);
  }
  map_device_page(GICD_PHYS);
  map_device_page(GICR_PHYS);
  map_device_page(GICR_SGI_PHYS);

  __asm__ volatile("dsb ishst\n"
                   "tlbi vmalle1\n"
                   "dsb ish\n"
                   "isb\n"
                   :
                   :
                   : "memory");
}

void finish_enter_el0(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t user_sp) {
  if (!cell_create_init(as, vmas, entry, user_sp)) {
    kprintf("[kernel] failed to create init cell\n");
    for (;;) {
      __asm__ volatile("wfe");
    }
  }
  syscall_set_address_space(cell_current_as());
  vmm_enable_ttbr0();
  vmm_install_user(cell_current_as());
  kprintf("[kernel] entering EL0\n");
  enter_el0(entry, user_sp);
}

void kernel_main(const struct spore_boot_info *boot_info) {
  if (boot_info == NULL || boot_info->magic != SPORE_BOOT_MAGIC || boot_info->version != SPORE_BOOT_VERSION ||
      boot_info->hhdm_offset == 0 || boot_info->memmap_phys == 0 || boot_info->modules_phys == 0) {
    for (;;) {
      __asm__ volatile("wfe");
    }
  }
  boot = boot_info;

  map_device_pages();
  pl011_init(boot->hhdm_offset);

  const struct spore_memmap_entry *memmap =
    (const struct spore_memmap_entry *)(uintptr_t)(boot->hhdm_offset + boot->memmap_phys);
  const struct spore_boot_module *modules =
    (const struct spore_boot_module *)(uintptr_t)(boot->hhdm_offset + boot->modules_phys);

  pmm_init(boot->hhdm_offset, memmap, boot->memmap_count);
  syscall_set_boot_time(boot->realtime_epoch_sec);
  cell_set_boot_epoch(boot->realtime_epoch_sec);
  random_init(boot->realtime_epoch_sec ^ boot->hhdm_offset ^ boot->memmap_phys ^ boot->modules_phys);
  kernel_log_handoff = true;
  (void)virtio_console_init(boot->hhdm_offset);
  kprintf("[kernel] booted at EL%u\n", (unsigned)current_el());
  exceptions_init();
  timer_init(boot->hhdm_offset);
  if (virtio_blk_init(boot->hhdm_offset)) { probe_ext2_root(); }
  if (virtio_net_init(boot->hhdm_offset)) {
    net_init();
    (void)virtio_net_smoke_tx();
  }
  cell_system_init(boot->hhdm_offset);

  ramfs_init(&boot_ramfs, modules, boot->module_count, boot->hhdm_offset);
  vfs_init(&boot_ramfs, root_ext2_ready ? &root_ext2 : NULL, boot->hhdm_offset);
  syscall_set_ramfs(&boot_ramfs);
  struct vfs_node init_node;
  struct elf_reader init_reader;
  if (!vfs_lookup("/sbin/init", &init_node) || !make_elf_reader(&init_node, &init_reader)) {
    kprintf("[kernel] missing /sbin/init\n");
    for (;;) {
      __asm__ volatile("wfe");
    }
  }
  kprintf("[kernel] loading /sbin/init\n");
  char interp_path[128];
  bool has_interp = elf_find_interp_aarch64(&init_reader, interp_path, sizeof(interp_path));

  struct user_address_space as;
  struct vma_list vmas;
  struct loaded_elf elf;
  uint64_t user_sp;
  vma_list_init(&vmas);
  if (!vmm_user_init(&as, boot->hhdm_offset) || !elf_load_aarch64(&as, &vmas, &init_reader, 0, &elf)) {
    kprintf("[kernel] failed to prepare /sbin/init\n");
    for (;;) {
      __asm__ volatile("wfe");
    }
  }
  if (has_interp) {
    struct vfs_node interp_node;
    struct elf_reader interp_reader;
    struct loaded_elf interp;
    if (!vfs_lookup(interp_path, &interp_node) || !make_elf_reader(&interp_node, &interp_reader) ||
        !elf_load_aarch64(&as, &vmas, &interp_reader, 0x0000006000000000ull, &interp)) {
      kprintf("[kernel] failed to load %s\n", interp_path);
      for (;;) {
        __asm__ volatile("wfe");
      }
    }
    elf.runtime_entry = interp.entry;
    elf.at_base = interp.load_base;
  }
  if (!build_initial_stack(&as, &elf, &user_sp)) {
    kprintf("[kernel] failed to prepare /sbin/init\n");
    for (;;) {
      __asm__ volatile("wfe");
    }
  }

  uint64_t kernel_sp = (uint64_t)(uintptr_t)(kernel_stack + sizeof(kernel_stack));
  switch_stack_and_finish(kernel_sp, &as, &vmas, elf.runtime_entry, user_sp);

  for (;;) {
    __asm__ volatile("wfe");
  }
}
