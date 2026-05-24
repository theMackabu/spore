#pragma once

#include "elf/loader.h"
#include "mm/vmm.h"

#include <stdint.h>

bool build_initial_stack(struct user_address_space *as, const struct loaded_elf *elf, uint64_t *stack_pointer);
bool build_initial_stack_args(struct user_address_space *as, const struct loaded_elf *elf, const char *const argv[],
                              uint64_t argc, const char *const envp[], uint64_t envc, uint64_t *stack_pointer);
