#pragma once

#include <stdbool.h>
#include <stdint.h>

struct trap_frame {
  alignas(16) uint64_t x[31];
  uint64_t sp_el0;
  uint64_t elr_el1;
  uint64_t spsr_el1;
  uint64_t esr_el1;
  uint64_t pad;
};

static_assert(sizeof(struct trap_frame) == 288, "AArch64 exception frame size");

struct fp_state {
  uint8_t q[32][16];
  uint64_t fpcr;
  uint64_t fpsr;
} __attribute__((aligned(16)));

struct aarch64_thread_context {
  struct fp_state fp;
  uint64_t thread_pointer;
};

void aarch64_save_context(struct aarch64_thread_context *context);
void aarch64_restore_context(const struct aarch64_thread_context *context);

static inline uint64_t trap_syscall_number(const struct trap_frame *frame) {
  return frame->x[8];
}

static inline uint64_t trap_user_stack(const struct trap_frame *frame) {
  return frame->sp_el0;
}

static inline uint64_t trap_syscall_arg(const struct trap_frame *frame, unsigned index) {
  return frame->x[index];
}

static inline void trap_set_result(struct trap_frame *frame, uint64_t value) {
  frame->x[0] = value;
}

static inline void trap_set_stack(struct trap_frame *frame, uint64_t sp) {
  frame->sp_el0 = sp;
}

void aarch64_init_user_frame(struct trap_frame *frame, uint64_t entry, uint64_t sp);
void aarch64_set_snapshot_entry(struct trap_frame *frame, uint64_t entry, uint64_t arg, uint64_t pid);
bool aarch64_restart_syscall(struct trap_frame *frame);
