#pragma once

#if !defined(SPORE_NATIVE_PORT)
#include "arch/aarch64/regs.h"
#else
#include <stdbool.h>
#include <stdint.h>
#ifdef __x86_64__
#include "arch/x86_64/syscall_frame.h"
static inline uint64_t trap_user_stack(const struct trap_frame *f) {
  return f->rsp;
}
static inline void trap_set_result(struct trap_frame *f, uint64_t value) {
  f->rax = value;
}
static inline void trap_set_stack(struct trap_frame *f, uint64_t sp) {
  f->rsp = sp;
}
#else
#include "arch/riscv64/syscall_frame.h"
static inline uint64_t trap_user_stack(const struct trap_frame *f) {
  return f->sp;
}
static inline void trap_set_result(struct trap_frame *f, uint64_t value) {
  f->a[0] = value;
}
static inline void trap_set_stack(struct trap_frame *f, uint64_t sp) {
  f->sp = sp;
}
#endif
struct arch_thread_context {
  uint64_t thread_pointer;
#ifdef __x86_64__
  alignas(16) uint8_t fp[512];
#else
  uint64_t fp[33];
#endif
};
void arch_save_context(struct arch_thread_context *context);
void arch_restore_context(const struct arch_thread_context *context);
void arch_init_user_frame(struct trap_frame *frame, uint64_t entry, uint64_t sp, bool freebsd);
void arch_set_snapshot_entry(struct trap_frame *frame, uint64_t entry, uint64_t arg, uint64_t pid);
bool arch_restart_syscall(struct trap_frame *frame);
#endif

static inline void arch_reset_context(struct arch_thread_context *context) {
  *context = (struct arch_thread_context){0};
#if defined(SPORE_NATIVE_PORT) && defined(__x86_64__)
  const uint16_t control_word = 0x037f;
  const uint32_t mxcsr = 0x1f80;
  __builtin_memcpy(context->fp, &control_word, sizeof(control_word));
  __builtin_memcpy(context->fp + 24, &mxcsr, sizeof(mxcsr));
#endif
  arch_restore_context(context);
}
