#pragma once

#include <stddef.h>
#include <stdint.h>

struct x86_64_syscall_frame {
  uint64_t rax, r10, r9, r8, rdx, rsi, rdi;
  uint64_t rip, cs, rflags, rsp, ss;
};

static_assert(offsetof(struct x86_64_syscall_frame, rflags) == 72);
static_assert(sizeof(struct x86_64_syscall_frame) == 96);
