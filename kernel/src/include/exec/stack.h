#pragma once

#include "elf/loader.h"
#include "mm/vmm.h"

#include <stdint.h>

enum {
  USER_STACK_TOP = 0x00007ffffff00000ull,
  USER_STACK_SIZE = 32 * 1024 * 1024,
};

struct exec_stack_credentials {
  uint32_t uid;
  uint32_t euid;
  uint32_t gid;
  uint32_t egid;
};

bool build_initial_stack(struct user_address_space *as, const struct loaded_elf *elf, uint64_t *stack_pointer);
bool build_initial_stack_args(struct user_address_space *as, const struct loaded_elf *elf, const char *const argv[],
                              uint64_t argc, const char *const envp[], uint64_t envc,
                              const struct exec_stack_credentials *creds, uint64_t *stack_pointer);
