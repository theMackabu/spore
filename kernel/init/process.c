#include "boot.h"
#include "init/app.h"
#include "init/process.h"
#include "abi/linux/shared/usercopy.h"
#ifdef SPORE_NATIVE_PORT
#include "arch/bringup/user.h"
#else
#include "arch/aarch64/exceptions.h"
#include "arch/aarch64/mmu.h"
#endif
#include "arch/smp.h"
#include "proc/thread.h"
#include "exec/elf.h"
#include "exec/stack.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "proc/bootstrap.h"
#include "proc/limits.h"
#include "proc/memory.h"

void boot_activate_process(struct first_process *first) {
  if (!cell_create_init(first->program, &first->as, &first->vmas, first->entry, first->sp)) {
    boot_fail(BOOT_INIT_FAILED, "failed to create init cell");
  }
}

static void enter_first_process(void *context) {
  struct first_process *first = context;
  boot_activate_process(first);
  arch_reset_context(&cell_current_thread_internal()->context);
  smp_start_scheduler_cpus();
  syscall_set_address_space(cell_current_as());
#ifndef SPORE_NATIVE_PORT
  aarch64_enable_ttbr0();
#endif
  vmm_install_user(cell_current_as());
  kprintf("[kernel] entering EL0\n");
  if (app_mode) { app_track_entry(first->entry); }
#ifdef SPORE_NATIVE_PORT
  user_enter(first->entry, first->sp);
#else
  bool bsd = cell_current_as()->freebsd_abi;
  enter_el0(first->entry, first->sp, bsd ? first->sp : 0, bsd ? first->entry : 0);
#endif
}

void boot_load_process(struct first_process *first, const struct init_program *program, uint64_t hhdm_offset) {

  struct vfs_node executable, cwd;
  struct elf_reader reader;
  kprintf("[kernel] loading %s\n", program->path);
  if (!vfs_lookup(program->path, &executable) || !elf_reader_from_vfs(&executable, &reader)) {
    boot_fail(BOOT_INIT_FAILED, "executable missing");
  }
  if (!vfs_lookup(program->cwd, &cwd) || !cwd.is_dir) { boot_fail(BOOT_INIT_FAILED, "working directory missing"); }

  first->program = program;
  vma_list_init(&first->vmas);
  if (!vmm_user_init(&first->as, hhdm_offset)) { boot_fail(BOOT_INIT_FAILED, "cannot prepare initial process"); }
  struct loaded_elf elf;
  char interp_path[CELL_PATH_MAX];
  enum elf_load_status status = elf_load_program(&first->as, &first->vmas, &reader, &elf, interp_path, sizeof(interp_path));
  if (status != ELF_LOADED) {
    boot_fail(BOOT_INIT_FAILED, status == ELF_BAD_INTERPRETER ? "invalid ELF interpreter" : "invalid ELF executable");
  }
  const struct exec_stack_credentials creds = {0};
  if (!build_initial_stack_args(&first->as, &elf, program->path, program->argv, program->argc, program->env,
                                program->envc, &creds, &first->sp)) {
    boot_fail(BOOT_INIT_FAILED, "cannot prepare initial process");
  }
  first->entry = elf.runtime_entry;
}

void boot_start_process(const struct init_program *program, uint64_t hhdm_offset) {
  struct first_process first;
  boot_load_process(&first, program, hhdm_offset);
#ifdef SPORE_NATIVE_PORT
  enter_first_process(&first);
  __builtin_unreachable();
#else
  static uint8_t kernel_stack[64 * 1024] __attribute__((aligned(16)));
  aarch64_call_on_stack((uintptr_t)(kernel_stack + sizeof(kernel_stack)), enter_first_process, &first);
#endif
}
