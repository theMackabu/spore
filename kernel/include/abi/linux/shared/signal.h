#pragma once

#include <stddef.h>
#include <stdint.h>

struct siginfo_fields64 {
  int32_t si_signo;
  int32_t si_errno;
  int32_t si_code;
  uint64_t si_addr;
};

struct siginfo64 {
  struct siginfo_fields64 fields;
  uint8_t pad[128 - sizeof(struct siginfo_fields64)];
};

struct stack_t64 {
  uint64_t ss_sp;
  int32_t ss_flags;
  uint32_t pad;
  uint64_t ss_size;
};
