#include "boot.h"
#include "abi/linux/aarch64/syscall.h"
#include "arch/aarch64/exceptions.h"
#include "arch/aarch64/platform.h"
#include "arch/aarch64/smp.h"
#include "arch/aarch64/timer.h"
#include "drivers/virtio_net.h"
#include "lib/kprintf.h"
#include "net/net.h"
#include "lib/random.h"
#include "mm/pmm.h"
#include "proc/accounting.h"
#include "proc/bootstrap.h"

void kernel_main(const struct spore_boot_info *boot) {
  if (boot == NULL || aarch64_current_el() != 1) { aarch64_halt(); }
  struct boot_profile profile = {0};
  boot_select_profile(boot, &profile);
  pmm_init(boot->hhdm_offset, boot->memmap, boot->memmap_count);
  smp_init_topology(boot->mp);
  aarch64_map_devices(boot->hhdm_offset);
  klog_init_console(boot->hhdm_offset, smp_present_cpu_count(), boot->mp ? "Limine" : "QEMU");

  syscall_set_boot_time(boot->realtime_epoch_sec);
  cell_set_boot_epoch(boot->realtime_epoch_sec);
  random_init(boot->realtime_epoch_sec ^ boot->hhdm_offset ^ (uintptr_t)boot->mp ^ (uintptr_t)boot->memmap);
  kprintf("[kernel] booted at EL%u\n", (unsigned)aarch64_current_el());
  exceptions_init();
  smp_boot_parked_secondaries();
  timer_init(boot->hhdm_offset);
  struct ext2_fs *disk = boot_mount_disk(boot->hhdm_offset);
  if (virtio_net_init(boot->hhdm_offset)) { net_init(); }
  cell_system_init();
  boot_mount_root(boot, disk, &profile);
  boot_start_process(&profile.program, boot->hhdm_offset);
}
