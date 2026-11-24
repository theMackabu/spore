#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct spore_elf_program {
  uint32_t type;
  uint32_t flags;
  uint64_t offset;
  uint64_t vaddr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
};

struct loaded_elf;

typedef bool (*spore_elf_read_fn)(void *ctx, uint64_t offset, void *dst, size_t len);
typedef bool (*spore_elf_map_fn)(void *ctx, const struct spore_elf_program *program, uint64_t segment_vaddr,
                                 uint64_t start, uint64_t end);

bool spore_elf_load(spore_elf_read_fn read_at, void *reader_ctx, uint64_t image_size, uint64_t load_base,
                    spore_elf_map_fn map, void *map_ctx, struct loaded_elf *out, char *interp_path, size_t path_cap);
