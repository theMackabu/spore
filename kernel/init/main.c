#include "abi/linux/aarch64/syscall.h"
#include "arch/aarch64/exceptions.h"
#include "arch/aarch64/mmu.h"
#include "arch/aarch64/platform.h"
#include "arch/aarch64/smp.h"
#include "arch/aarch64/timer.h"
#include "init/boot_info.h"
#include "proc/accounting.h"
#include "proc/bootstrap.h"
#include "exec/elf.h"
#include "exec/stack.h"
#include "fs/ext2.h"
#include "lib/kprintf.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "net/net.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "fs/ramfs.h"
#include "lib/random.h"
#include "fs/vfs.h"
#include "drivers/virtio_blk.h"
#include "drivers/virtio_net.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static uint8_t kernel_stack[64 * 1024] __attribute__((aligned(16)));
static struct ramfs boot_ramfs;
static struct ext2_fs root_ext2;

static bool ext2_blk_read(void *ctx, uint64_t offset, void *dst, uint32_t len) {
  (void)ctx;
  return virtio_blk_read(offset, dst, len);
}

static bool ext2_blk_write(void *ctx, uint64_t offset, const void *src, uint32_t len) {
  (void)ctx;
  return virtio_blk_write(offset, src, len);
}

void finish_enter_el0(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t user_sp) {
  if (!cell_create_init(as, vmas, entry, user_sp)) {
    kprintf("[kernel] failed to create init cell\n");
    aarch64_halt();
  }
  smp_start_scheduler_cpus();
  syscall_set_address_space(cell_current_as());
  aarch64_enable_ttbr0();
  vmm_install_user(cell_current_as());
  kprintf("[kernel] entering EL0\n");
  enter_el0(entry, user_sp);
}

static void start_init(uint64_t hhdm_offset) {
  struct vfs_node init_node;
  struct elf_reader init_reader;
  if (!vfs_lookup("/sbin/init", &init_node) || !elf_reader_from_vfs(&init_node, &init_reader)) {
    kprintf("[kernel] missing /sbin/init\n");
    aarch64_halt();
  }
  kprintf("[kernel] loading /sbin/init\n");
  char interp_path[CELL_PATH_MAX];

  struct user_address_space as;
  struct vma_list vmas;
  struct loaded_elf elf;
  uint64_t user_sp;
  vma_list_init(&vmas);
  if (!vmm_user_init(&as, hhdm_offset)) {
    kprintf("[kernel] failed to prepare /sbin/init\n");
    aarch64_halt();
  }
  enum elf_load_status status = elf_load_program(&as, &vmas, &init_reader, &elf, interp_path, sizeof(interp_path));
  if (status != ELF_LOADED) {
    kprintf("[kernel] failed to load %s\n", status == ELF_BAD_INTERPRETER ? interp_path : "/sbin/init");
    aarch64_halt();
  }
  if (!build_initial_stack(&as, &elf, &user_sp)) {
    kprintf("[kernel] failed to prepare /sbin/init\n");
    aarch64_halt();
  }

  uint64_t kernel_sp = (uint64_t)(uintptr_t)(kernel_stack + sizeof(kernel_stack));
  switch_stack_and_finish(kernel_sp, &as, &vmas, elf.runtime_entry, user_sp);

  aarch64_halt();
}

void kernel_main(void) {
  const struct spore_boot_info *boot = limine_boot_info();
  if (boot == NULL || aarch64_current_el() != 1) { aarch64_halt(); }
  pmm_init(boot->hhdm_offset, boot->memmap, boot->memmap_count);
  smp_init_topology(boot->mp);
  aarch64_map_devices(boot->hhdm_offset);
  klog_init_console(boot->hhdm_offset, smp_present_cpu_count());

  syscall_set_boot_time(boot->realtime_epoch_sec);
  cell_set_boot_epoch(boot->realtime_epoch_sec);
  random_init(boot->realtime_epoch_sec ^ boot->hhdm_offset ^ (uintptr_t)boot->mp ^ (uintptr_t)boot->memmap);
  kprintf("[kernel] booted at EL%u\n", (unsigned)aarch64_current_el());
  exceptions_init();
  smp_boot_parked_secondaries();
  timer_init(boot->hhdm_offset);
  struct ext2_fs *root = NULL;
  if (virtio_blk_init(boot->hhdm_offset)) {
    if (ext2_mount_rw(&root_ext2, ext2_blk_read, ext2_blk_write, NULL)) {
      root = &root_ext2;
    } else {
      kprintf("[spore] ext2: mount failed\n");
    }
  }
  if (virtio_net_init(boot->hhdm_offset)) { net_init(); }
  cell_system_init(boot->hhdm_offset);

  ramfs_init(&boot_ramfs, boot->modules, boot->module_count, boot->hhdm_offset);
  vfs_init(&boot_ramfs, root, boot->hhdm_offset);
  start_init(boot->hhdm_offset);
}
