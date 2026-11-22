#include "arch/aarch64/exceptions.h"
#include "arch/aarch64/smp.h"
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
  GICR_STRIDE = 0x20000,
};

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

static void boot_halt(void) {
  for (;;) {
    __asm__ volatile("wfe");
  }
}

static void invalidate_boot_mapping(uint64_t *entry) {
  *entry = 0;
  __asm__ volatile("dsb ishst\ntlbi vmalle1is\ndsb ish\nisb" : : : "memory");
}

// Limine maps normal RAM. Install QEMU MMIO in the holes of its direct map,
// preserving adjacent pages if firmware described the region as mapped.
static void map_device_page(uint64_t pa) {
  const uint64_t pa_mask = 0x0000fffffffff000ull;
  uint64_t va = boot->hhdm_offset + pa;
  uint64_t ttbr1;
  __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
  uint64_t *table = pmm_phys_to_virt(ttbr1 & pa_mask);

  for (unsigned shift = 39; shift > 12; shift -= 9) {
    uint64_t *entry = &table[(va >> shift) & 511];
    uint64_t desc = *entry;
    if ((desc & 3) != 3) {
      uint64_t next_pa = pmm_alloc_zero_page();
      if (next_pa == 0) { boot_halt(); }
      if (desc & 1) {
        uint64_t *next = pmm_phys_to_virt(next_pa);
        uint64_t stride = 1ull << (shift - 9);
        uint64_t base = (desc & pa_mask) & ~((1ull << shift) - 1);
        uint64_t attrs = desc & ~(pa_mask | 3ull | (1ull << 52));
        for (size_t i = 0; i < 512; ++i) {
          next[i] = (base + i * stride) | attrs | (shift == 21 ? 3ull : 1ull);
        }
        invalidate_boot_mapping(entry);
      }
      desc = next_pa | 3;
      *entry = desc;
      __asm__ volatile("dsb ishst" : : : "memory");
    }
    table = pmm_phys_to_virt(desc & pa_mask);
  }
  uint64_t *pte = &table[(va >> 12) & 511];
  if (*pte & 1) { invalidate_boot_mapping(pte); }
  *pte = pa | (2ull << 2) | (3ull << 8) | (1ull << 10) | (1ull << 53) | (1ull << 54) | 3;
}

static void map_device_pages(void) {
  uint64_t mair;
  __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
  mair &= ~(0xffull << 16); // Attr2: Device-nGnRnE; Limine uses Attr0 and Attr1.
  __asm__ volatile("msr mair_el1, %0\nisb" : : "r"(mair) : "memory");
  map_device_page(PL011_PHYS);
  for (size_t i = 0; i < VIRTIO_MMIO_PAGES; ++i) {
    map_device_page(VIRTIO_MMIO_PHYS + i * PAGE_SIZE);
  }
  map_device_page(GICD_PHYS);
  for (uint32_t cpu = 0; cpu < smp_present_cpu_count(); ++cpu) {
    map_device_page(GICR_PHYS + cpu * GICR_STRIDE);
    map_device_page(GICR_SGI_PHYS + cpu * GICR_STRIDE);
  }
  __asm__ volatile("dsb ishst\ntlbi vmalle1is\ndsb ish\nisb" : : : "memory");
}

void finish_enter_el0(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t user_sp) {
  if (!cell_create_init(as, vmas, entry, user_sp)) {
    kprintf("[kernel] failed to create init cell\n");
    for (;;) {
      __asm__ volatile("wfe");
    }
  }
  smp_start_scheduler_cpus();
  syscall_set_address_space(cell_current_as());
  vmm_enable_ttbr0();
  vmm_install_user(cell_current_as());
  kprintf("[kernel] entering EL0\n");
  enter_el0(entry, user_sp);
}

void kernel_main(void) {
  boot = limine_boot_info();
  if (boot == NULL || current_el() != 1) { boot_halt(); }
  pmm_init(boot->hhdm_offset, boot->memmap, boot->memmap_count);
  smp_init_topology(boot->mp);
  map_device_pages();
  pl011_init(boot->hhdm_offset);
  kprintf("[kernel] Limine handoff at EL1, %u CPU(s)\n", smp_present_cpu_count());
  kernel_log_handoff = true;
  (void)virtio_console_init(boot->hhdm_offset);

  syscall_set_boot_time(boot->realtime_epoch_sec);
  cell_set_boot_epoch(boot->realtime_epoch_sec);
  random_init(boot->realtime_epoch_sec ^ boot->hhdm_offset ^ (uintptr_t)boot->mp ^ (uintptr_t)boot->memmap);
  kprintf("[kernel] booted at EL%u\n", (unsigned)current_el());
  exceptions_init();
  smp_boot_parked_secondaries();
  timer_init(boot->hhdm_offset);
  if (virtio_blk_init(boot->hhdm_offset)) { probe_ext2_root(); }
  if (virtio_net_init(boot->hhdm_offset)) {
    net_init();
    (void)virtio_net_smoke_tx();
  }
  cell_system_init(boot->hhdm_offset);

  ramfs_init(&boot_ramfs, boot->modules, boot->module_count, boot->hhdm_offset);
  vfs_init(&boot_ramfs, root_ext2_ready ? &root_ext2 : NULL, boot->hhdm_offset);
  syscall_set_ramfs(&boot_ramfs);
  struct vfs_node init_node;
  struct elf_reader init_reader;
  if (!vfs_lookup("/sbin/init", &init_node) || !elf_reader_from_vfs(&init_node, &init_reader)) {
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
  if (!vmm_user_init(&as, boot->hhdm_offset) ||
      !elf_load_aarch64(&as, &vmas, &init_reader, 0x0000004000000000ull, &elf)) {
    kprintf("[kernel] failed to prepare /sbin/init\n");
    for (;;) {
      __asm__ volatile("wfe");
    }
  }
  if (has_interp) {
    struct vfs_node interp_node;
    struct elf_reader interp_reader;
    struct loaded_elf interp;
    if (!vfs_lookup(interp_path, &interp_node) || !elf_reader_from_vfs(&interp_node, &interp_reader) ||
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
