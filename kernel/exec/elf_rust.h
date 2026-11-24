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

struct spore_elf_loaded {
  uint64_t entry;
  uint64_t load_base;
  uint64_t phdr;
  uint64_t brk_base;
  uint16_t phent;
  uint16_t phnum;
};

typedef bool (*spore_elf_read_fn)(void *ctx, uint64_t offset, void *dst, size_t len);
typedef bool (*spore_elf_map_fn)(void *ctx, const struct spore_elf_program *program, uint64_t segment_vaddr,
                                 uint64_t start, uint64_t end);

bool spore_elf_load(spore_elf_read_fn read_at, void *reader_ctx, uint64_t image_size, uint64_t load_base,
                    spore_elf_map_fn map, void *map_ctx, struct spore_elf_loaded *out);
bool spore_elf_find_interp(spore_elf_read_fn read_at, void *reader_ctx, uint64_t image_size, char *out, size_t out_cap);
