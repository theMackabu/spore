#include "arch/aarch64/regs.h"

void arch_fp_save(struct fp_state *state);
void arch_fp_restore(const struct fp_state *state);

void arch_save_context(struct arch_thread_context *context) {
  arch_fp_save(&context->fp);
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(context->thread_pointer));
}

void arch_restore_context(const struct arch_thread_context *context) {
  arch_fp_restore(&context->fp);
  __asm__ volatile("msr tpidr_el0, %0" : : "r"(context->thread_pointer));
}
