#include "elf/loader.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
  EI_NIDENT = 16,
  ET_EXEC = 2,
  EM_AARCH64 = 183,
  PT_LOAD = 1,
  PF_R = 4,
  PF_W = 2,
  PAGE_SIZE = 4096,
  PAGE_COUNT = 8,
};

struct elf64_ehdr {
  unsigned char e_ident[EI_NIDENT];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
};

struct elf64_phdr {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
};

struct mapping {
  uint64_t va;
  uint64_t pa;
  uint32_t flags;
};

static uint8_t phys[(PAGE_COUNT + 1) * PAGE_SIZE];
static uint64_t next_pa = PAGE_SIZE;
static struct mapping mappings[PAGE_COUNT];
static size_t mapping_count;

uint64_t pmm_alloc_zero_page(void) {
  assert(next_pa < sizeof(phys));
  uint64_t pa = next_pa;
  memset(&phys[pa], 0, PAGE_SIZE);
  next_pa += PAGE_SIZE;
  return pa;
}

bool vmm_map_page(struct user_address_space *as, uint64_t va, uint64_t pa, uint32_t flags) {
  (void)as;
  assert(mapping_count < PAGE_COUNT);
  mappings[mapping_count++] = (struct mapping){.va = va, .pa = pa, .flags = flags};
  return true;
}

static uint64_t phys_for(uint64_t va) {
  for (size_t i = 0; i < mapping_count; ++i) {
    if (mappings[i].va == (va & ~(uint64_t)(PAGE_SIZE - 1))) { return mappings[i].pa + (va & (PAGE_SIZE - 1)); }
  }
  return 0;
}

struct memory_reader {
  const uint8_t *data;
  size_t size;
};

static bool memory_read_at(void *ctx, uint64_t offset, void *dst, size_t len) {
  const struct memory_reader *reader = ctx;
  if (reader == NULL || offset > reader->size || len > reader->size - offset) { return false; }
  memcpy(dst, reader->data + offset, len);
  return true;
}

static void make_test_elf(uint8_t *image, size_t image_size) {
  memset(image, 0, image_size);
  struct elf64_ehdr *eh = (struct elf64_ehdr *)image;
  memcpy(eh->e_ident, "\177ELF", 4);
  eh->e_ident[4] = 2;
  eh->e_ident[5] = 1;
  eh->e_type = ET_EXEC;
  eh->e_machine = EM_AARCH64;
  eh->e_version = 1;
  eh->e_entry = 0x401000;
  eh->e_phoff = sizeof(*eh);
  eh->e_ehsize = sizeof(*eh);
  eh->e_phentsize = sizeof(struct elf64_phdr);
  eh->e_phnum = 2;

  struct elf64_phdr *ph = (struct elf64_phdr *)(image + eh->e_phoff);
  ph[0] = (struct elf64_phdr){
    .p_type = PT_LOAD,
    .p_flags = PF_R,
    .p_offset = 0,
    .p_vaddr = 0x400000,
    .p_filesz = 0x100,
    .p_memsz = 0x100,
    .p_align = PAGE_SIZE,
  };
  ph[1] = (struct elf64_phdr){
    .p_type = PT_LOAD,
    .p_flags = PF_R | PF_W,
    .p_offset = 0x200,
    .p_vaddr = 0x401000,
    .p_filesz = 3,
    .p_memsz = 8,
    .p_align = 1,
  };
  memcpy(image + 0x200, "abc", 3);
}

int main(void) {
  uint8_t image[0x300];
  make_test_elf(image, sizeof(image));

  struct user_address_space as = {.hhdm_offset = (uint64_t)(uintptr_t)phys};
  struct loaded_elf loaded = {0};
  struct memory_reader mem = {.data = image, .size = sizeof(image)};
  struct elf_reader reader = {.read_at = memory_read_at, .ctx = &mem, .size = sizeof(image)};
  assert(elf_load_aarch64(&as, &reader, 0, &loaded));

  assert(loaded.entry == 0x401000);
  assert(loaded.phdr == 0x400040);
  assert(loaded.phent == sizeof(struct elf64_phdr));
  assert(loaded.phnum == 2);
  assert(loaded.brk_base == 0x402000);
  assert(as.brk_base == loaded.brk_base);

  uint64_t data_pa = phys_for(0x401000);
  assert(data_pa != 0);
  assert(memcmp(&phys[data_pa], "abc\0\0\0\0\0", 8) == 0);

  image[18] = 0;
  assert(!elf_load_aarch64(&as, &reader, 0, &loaded));
  return 0;
}
