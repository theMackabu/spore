#pragma once

#include "mm/vmm.h"
#include "ramfs.h"

#include <stdint.h>

void exceptions_init(void);
void syscall_set_address_space(struct user_address_space *as);
void syscall_set_ramfs(struct ramfs *fs);
void enter_el0(uint64_t entry, uint64_t sp);
void switch_stack_and_finish(uint64_t kernel_sp, struct user_address_space *as, uint64_t entry, uint64_t user_sp);
