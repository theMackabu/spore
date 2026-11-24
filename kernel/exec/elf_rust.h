#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct spore_elf_header {
  uint16_t type;
  uint16_t phentsize;
  uint16_t phnum;
  uint64_t entry;
  uint64_t phoff;
};

struct spore_elf_program {
  uint32_t type;
  uint32_t flags;
  uint64_t offset;
  uint64_t vaddr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
};

bool spore_elf_parse_header(const uint8_t *data, size_t len, uint64_t image_size, struct spore_elf_header *out);
bool spore_elf_parse_program(const uint8_t *data, size_t len, struct spore_elf_program *out);
bool spore_elf_interp_valid(const uint8_t *data, size_t len);
