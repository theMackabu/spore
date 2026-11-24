#include "arch/clock.h"
#include "arch/entropy.h"
#include "arch/regs.h"

extern void riscv_save_fp(uint64_t *fp), riscv_restore_fp(const uint64_t *fp);
void arch_save_context(struct arch_thread_context *c) {
  __asm__ volatile("mv %0,tp" : "=r"(c->thread_pointer));
  riscv_save_fp(c->fp);
}
void arch_restore_context(const struct arch_thread_context *c) {
  __asm__ volatile("mv tp,%0" ::"r"(c->thread_pointer));
  riscv_restore_fp(c->fp);
}
void arch_init_user_frame(struct trap_frame *f, uint64_t pc, uint64_t sp, bool bsd) {
  *f = (struct trap_frame){.pc = pc, .sp = sp, .ra = bsd ? pc : 0};
  if (bsd) f->a[0] = sp;
}
void arch_set_snapshot_entry(struct trap_frame *f, uint64_t pc, uint64_t arg, uint64_t pid) {
  f->pc = pc;
  f->a[0] = arg;
  f->a[1] = pid;
}
bool arch_restart_syscall(struct trap_frame *f) {
  if (f->pc < 4) return false;
  f->pc -= 4;
  return true;
}
uint64_t arch_counter_value(void) {
  uint64_t time;
  __asm__ volatile("rdtime %0" : "=r"(time));
  return time;
}
uint64_t arch_counter_frequency(void) {
  return 10000000;
}
bool arch_rndr_supported(void) {
  return false;
}
bool arch_rndr64(uint64_t *out) {
  (void)out;
  return false;
}
