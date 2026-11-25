#pragma once

#include "init/program.h"
#include "mm/vma.h"
#include "mm/vmm.h"

struct first_process {
  const struct init_program *program;
  struct user_address_space as;
  struct vma_list vmas;
  uint64_t entry, sp;
};

void boot_load_process(struct first_process *first, const struct init_program *program, uint64_t hhdm_offset);
void boot_activate_process(struct first_process *first);
