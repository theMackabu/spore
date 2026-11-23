#pragma once

#include "mm/vma.h"
#include "mm/vmm.h"

#include <stdint.h>

void exceptions_init(void);
void enter_el0(uint64_t entry, uint64_t sp);
void switch_stack_and_finish(uint64_t kernel_sp, struct user_address_space *as, struct vma_list *vmas, uint64_t entry,
                             uint64_t user_sp);
