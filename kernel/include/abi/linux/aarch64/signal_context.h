#pragma once

#include "abi/linux/shared/signal.h"

constexpr uint32_t AARCH64_FPSIMD_MAGIC = 0x46508001;
struct aarch64_ctx_header {
  uint32_t magic, size;
};
struct aarch64_fpsimd_ctx {
  struct aarch64_ctx_header head;
  uint32_t fpsr, fpcr;
  uint8_t vregs[32][16];
};
static_assert(sizeof(struct aarch64_fpsimd_ctx) == 528);

struct sigcontext64 {
  uint64_t fault_address;
  uint64_t regs[31];
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
  uint8_t pad[8];
  uint8_t reserved[4096];
};

struct ucontext64 {
  uint64_t uc_flags;
  uint64_t uc_link;
  struct stack_t64 uc_stack;
  uint64_t uc_sigmask;
  uint8_t pad[(1024 - 64) / 8];
  uint8_t pad2[8];
  struct sigcontext64 uc_mcontext;
};

static_assert(offsetof(struct siginfo64, fields.si_addr) == 16, "linux arm64 siginfo si_addr offset");
static_assert(offsetof(struct ucontext64, uc_mcontext) == 176, "linux arm64 ucontext mcontext offset");
static_assert(offsetof(struct sigcontext64, regs) == 8, "linux arm64 sigcontext regs offset");
static_assert(offsetof(struct sigcontext64, sp) == 256, "linux arm64 sigcontext sp offset");
static_assert(offsetof(struct sigcontext64, pc) == 264, "linux arm64 sigcontext pc offset");
static_assert(offsetof(struct sigcontext64, pstate) == 272, "linux arm64 sigcontext pstate offset");
