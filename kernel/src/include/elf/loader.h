#pragma once

#include "mm/vmm.h"
#include "mm/vma.h"
#include "vfs.h"

#include <stddef.h>
#include <stdint.h>

typedef bool (*elf_read_at_fn)(void *ctx, uint64_t offset, void *dst, size_t len);

struct elf_reader {
  elf_read_at_fn read_at;
  void *ctx;
  const struct vfs_node *node;
  uint64_t size;
};

struct loaded_elf {
  uint64_t entry;
  uint64_t runtime_entry;
  uint64_t load_base;
  uint64_t at_base;
  uint64_t phdr;
  uint16_t phent;
  uint16_t phnum;
  uint64_t brk_base;
};

bool elf_load_aarch64(struct user_address_space *as, struct vma_list *vmas, const struct elf_reader *reader,
                      uint64_t load_base, struct loaded_elf *out);
bool elf_find_interp_aarch64(const struct elf_reader *reader, char *out, size_t out_cap);
