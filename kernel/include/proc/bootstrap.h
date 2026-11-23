#pragma once

#include <stdbool.h>
#include <stdint.h>

struct user_address_space;
struct vma_list;

void cell_system_init(uint64_t hhdm_offset);
bool cell_create_init(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t sp);
