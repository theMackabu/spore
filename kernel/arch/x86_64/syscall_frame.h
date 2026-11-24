#pragma once

#include <stddef.h>
#include <stdint.h>

struct trap_frame {
  uint64_t rax, r10, r9, r8, rdx, rsi, rdi;
  uint64_t rcx, r11, rbx, rbp, r12, r13, r14, r15;
  uint64_t rip, cs, rflags, rsp, ss;
};

static_assert(offsetof(struct trap_frame, rflags) == 136);
static_assert(sizeof(struct trap_frame) == 160);
