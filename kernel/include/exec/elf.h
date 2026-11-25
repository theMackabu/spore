#pragma once

#include "fs/vfs.h"
#include "mm/vma.h"
#include "mm/vmm.h"

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
  uint8_t osabi;
  uint64_t brk_base;
};

bool elf_reader_from_vfs(const struct vfs_node *node, struct elf_reader *reader);
bool elf_map_image(struct vma_list *vmas, const struct elf_reader *reader, uint64_t load_base, struct loaded_elf *out,
                   char *interp_path, size_t path_cap);

enum elf_load_status { ELF_LOADED, ELF_BAD_EXECUTABLE, ELF_BAD_INTERPRETER };
enum elf_load_status elf_load_program(struct user_address_space *as, struct vma_list *vmas,
                                      const struct elf_reader *reader, struct loaded_elf *out, char *interp_path,
                                      size_t path_cap);
