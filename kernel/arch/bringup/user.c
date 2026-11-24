#include "user.h"
#include "arch/clock.h"
#include "arch/regs.h"
#include "drivers/virtio_net.h"
#include "init/app.h"
#include "init/boot.h"
#include "lib/kprintf.h"
#include "lib/random.h"
#include "net/net.h"
#include "platform.h"
#include "proc/accounting.h"
#include "proc/bootstrap.h"
#include "proc/memory.h"

bool user_freebsd;
#ifdef __x86_64__
#define linux_entry linux_x86_64_syscall
#define freebsd_entry freebsd_x86_64_syscall
#else
#define linux_entry linux_riscv64_syscall
#define freebsd_entry freebsd_riscv64_syscall
#endif
void linux_entry(struct trap_frame *);
void freebsd_entry(struct trap_frame *);
void port_syscall(struct trap_frame *frame) {
  if (cell_current_as()->freebsd_abi) {
    freebsd_entry(frame);
  } else {
    linux_entry(frame);
  }
}

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

[[noreturn]] void user_finish(char kind, unsigned value) {
  app_finish(kind, value);
}

[[noreturn]] void user_start(const struct spore_boot_info *boot) {
  root = pmm_phys_to_virt(active_root());

  struct boot_profile profile = {0};
  boot_select_profile(boot, &profile);
  if (!boot->module_count && app_mode) { port_monitor(); }
  if (!status_init(boot->hhdm_offset) && app_mode) { platform_poweroff(); }
  if (app_mode) { klog_handoff(); }
  syscall_set_boot_time(boot->realtime_epoch_sec);
  cell_set_boot_epoch(boot->realtime_epoch_sec);
  random_init(boot->realtime_epoch_sec ^ boot->hhdm_offset);
#ifdef __x86_64__
  extern void virtio_pci_init(void);
  extern bool x86_map_timer(void);
  if (!x86_map_timer()) { boot_fail(BOOT_INIT_FAILED, "local APIC unavailable"); }
  virtio_pci_init();
#endif
  struct ext2_fs *disk = boot_mount_disk(boot->hhdm_offset);
  if (!profile.payload && !disk) { port_monitor(); }
  if (virtio_net_init(boot->hhdm_offset)) { net_init(); }
  cell_system_init();
  boot_mount_root(boot, disk, &profile);
  boot_start_process(&profile.program, boot->hhdm_offset);
}
