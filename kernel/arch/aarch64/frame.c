#include "arch/aarch64/regs.h"

void arch_init_user_frame(struct trap_frame *frame, uint64_t entry, uint64_t sp, bool freebsd) {
  *frame = (struct trap_frame){.elr_el1 = entry, .sp_el0 = sp, .spsr_el1 = 0x340};
  if (freebsd) { frame->x[0] = sp; frame->x[30] = entry; }
}

void arch_set_snapshot_entry(struct trap_frame *frame, uint64_t entry, uint64_t arg, uint64_t pid) {
  frame->elr_el1 = entry;
  frame->x[0] = arg;
  frame->x[1] = pid;
  frame->spsr_el1 &= ~(1ull << 7);
}

bool arch_restart_syscall(struct trap_frame *frame) {
  if (frame->elr_el1 < 4) { return false; }
  frame->elr_el1 -= 4;
  return true;
}
