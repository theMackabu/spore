#pragma once

#include <stddef.h>
#include <stdint.h>

struct signal_sender {
  int32_t pid;
  uint32_t uid;
};

struct siginfo_fields64 {
  int32_t si_signo;
  int32_t si_errno;
  int32_t si_code;
  union {
    uint64_t si_addr;
    struct signal_sender sender;
  };
  int32_t child_status;
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
