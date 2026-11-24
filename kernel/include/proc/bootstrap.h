#pragma once

#include <stdbool.h>
#include <stdint.h>

struct user_address_space;
struct vma_list;
struct init_program;

void cell_system_init(void);
bool cell_create_init(const struct init_program *program, struct user_address_space *as, struct vma_list *vmas,
                      uint64_t entry, uint64_t sp);
