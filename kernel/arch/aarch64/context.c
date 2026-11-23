#include "arch/aarch64/regs.h"

void arch_fp_save(struct fp_state *state);
void arch_fp_restore(const struct fp_state *state);

void aarch64_save_context(struct aarch64_thread_context *context) {
  arch_fp_save(&context->fp);
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(context->thread_pointer));
}

void aarch64_restore_context(const struct aarch64_thread_context *context) {
  arch_fp_restore(&context->fp);
  __asm__ volatile("msr tpidr_el0, %0" : : "r"(context->thread_pointer));
}

void aarch64_init_user_frame(struct trap_frame *frame, uint64_t entry, uint64_t sp) {
  *frame = (struct trap_frame){.elr_el1 = entry, .sp_el0 = sp, .spsr_el1 = 0x340};
}

void aarch64_set_snapshot_entry(struct trap_frame *frame, uint64_t entry, uint64_t arg, uint64_t pid) {
  frame->elr_el1 = entry;
  frame->x[0] = arg;
  frame->x[1] = pid;
  frame->spsr_el1 &= ~(1ull << 7);
}

bool aarch64_restart_syscall(struct trap_frame *frame) {
  if (frame->elr_el1 < 4) { return false; }
  frame->elr_el1 -= 4;
  return true;
}
