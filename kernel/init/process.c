#include "boot.h"
#include "init/app.h"
#include "abi/linux/shared/usercopy.h"
#include "arch/aarch64/exceptions.h"
#include "arch/aarch64/mmu.h"
#include "arch/aarch64/smp.h"
#include "exec/elf.h"
#include "exec/stack.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "proc/bootstrap.h"
#include "proc/limits.h"
#include "proc/memory.h"

struct first_process {
  const struct init_program *program;
  struct user_address_space as;
  struct vma_list vmas;
  uint64_t entry, sp;
};

static void enter_first_process(void *context) {
  struct first_process *first = context;
  if (!cell_create_init(first->program, &first->as, &first->vmas, first->entry, first->sp)) {
    boot_fail(BOOT_INIT_FAILED, "failed to create init cell");
  }
  smp_start_scheduler_cpus();
  syscall_set_address_space(cell_current_as());
  aarch64_enable_ttbr0();
  vmm_install_user(cell_current_as());
  kprintf("[kernel] entering EL0\n");
  if (app_mode) { app_track_entry(first->entry); }
  bool bsd = cell_current_as()->freebsd_abi;
  enter_el0(first->entry, first->sp, bsd ? first->sp : 0, bsd ? first->entry : 0);
}

void boot_start_process(const struct init_program *program, uint64_t hhdm_offset) {
  static uint8_t kernel_stack[64 * 1024] __attribute__((aligned(16)));
  struct vfs_node executable, cwd;
  struct elf_reader reader;
  kprintf("[kernel] loading %s\n", program->path);
  if (!vfs_lookup(program->path, &executable) || !elf_reader_from_vfs(&executable, &reader)) {
    boot_fail(BOOT_INIT_FAILED, "executable missing");
  }
  if (!vfs_lookup(program->cwd, &cwd) || !cwd.is_dir) { boot_fail(BOOT_INIT_FAILED, "working directory missing"); }

  struct first_process first;
  first.program = program;
  vma_list_init(&first.vmas);
  if (!vmm_user_init(&first.as, hhdm_offset)) { boot_fail(BOOT_INIT_FAILED, "cannot prepare initial process"); }
  struct loaded_elf elf;
  char interp_path[CELL_PATH_MAX];
  enum elf_load_status status = elf_load_program(&first.as, &first.vmas, &reader, &elf, interp_path, sizeof(interp_path));
  if (status != ELF_LOADED) {
    boot_fail(BOOT_INIT_FAILED, status == ELF_BAD_INTERPRETER ? "invalid ELF interpreter" : "invalid ELF executable");
  }
  const struct exec_stack_credentials creds = {0};
  if (!build_initial_stack_args(&first.as, &elf, program->path, program->argv, program->argc, program->env,
                                program->envc, &creds, &first.sp)) {
    boot_fail(BOOT_INIT_FAILED, "cannot prepare initial process");
  }
  first.entry = elf.runtime_entry;
  aarch64_call_on_stack((uintptr_t)(kernel_stack + sizeof(kernel_stack)), enter_first_process, &first);
}
