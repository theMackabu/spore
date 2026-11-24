#pragma once

#include <stddef.h>
#include <stdint.h>

struct riscv64_syscall_frame {
  uint64_t ra, t[7], a[8];
};

static_assert(offsetof(struct riscv64_syscall_frame, a) == 64);
static_assert(sizeof(struct riscv64_syscall_frame) == 128);
