#include "elf/loader.h"

#include "mem.h"
#include "mm/pmm.h"

enum {
  EI_NIDENT = 16,
  ET_EXEC = 2,
  ET_DYN = 3,
  EM_AARCH64 = 183,
  PT_LOAD = 1,
  PF_X = 1,
  PF_W = 2,
  PF_R = 4,
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

static uint64_t align_down(uint64_t value, uint64_t align) {
  return value & ~(align - 1);
}

static uint64_t align_up(uint64_t value, uint64_t align) {
  return (value + align - 1) & ~(align - 1);
}

static bool bounds_ok(uint64_t off, uint64_t len, size_t image_size) {
  return off <= image_size && len <= image_size - off;
}

static uint32_t phdr_to_vmm_flags(uint32_t flags) {
  uint32_t out = VMM_USER_READ;
  if ((flags & PF_W) != 0) { out |= VMM_USER_WRITE; }
  if ((flags & PF_X) != 0) { out |= VMM_USER_EXEC; }
  return out;
}

bool elf_load_static_aarch64(struct user_address_space *as, const void *image, size_t image_size,
                             struct loaded_elf *out) {
  if (image_size < sizeof(struct elf64_ehdr)) { return false; }

  const struct elf64_ehdr *ehdr = image;
  const unsigned char magic[4] = {0x7f, 'E', 'L', 'F'};
  if (kmemcmp(ehdr->e_ident, magic, sizeof(magic)) != 0 || ehdr->e_ident[4] != 2 || ehdr->e_ident[5] != 1 ||
      (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) || ehdr->e_machine != EM_AARCH64 ||
      ehdr->e_phentsize != sizeof(struct elf64_phdr) ||
      !bounds_ok(ehdr->e_phoff, (uint64_t)ehdr->e_phentsize * ehdr->e_phnum, image_size)) {
    return false;
  }

  const struct elf64_phdr *phdrs = (const void *)((const uint8_t *)image + ehdr->e_phoff);
  uint64_t high = 0;
  uint64_t phdr_va = 0;

  for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
    const struct elf64_phdr *ph = &phdrs[i];
    if (ph->p_type != PT_LOAD) { continue; }
    if (ph->p_memsz < ph->p_filesz || !bounds_ok(ph->p_offset, ph->p_filesz, image_size) ||
        (ph->p_align > PAGE_SIZE && (ph->p_vaddr % ph->p_align) != (ph->p_offset % ph->p_align))) {
      return false;
    }

    uint64_t start = align_down(ph->p_vaddr, PAGE_SIZE);
    uint64_t end = align_up(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
    uint32_t flags = phdr_to_vmm_flags(ph->p_flags);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
      uint64_t pa = pmm_alloc_zero_page();
      if (pa == 0 || !vmm_map_page(as, va, pa, flags | VMM_USER_WRITE)) { return false; }

      uint64_t page_file_start = 0;
      uint64_t page_file_len = 0;
      uint64_t seg_page_start = va > ph->p_vaddr ? va : ph->p_vaddr;
      uint64_t seg_file_end = ph->p_vaddr + ph->p_filesz;
      if (seg_page_start < seg_file_end) {
        uint64_t seg_page_end = va + PAGE_SIZE;
        uint64_t copy_end = seg_page_end < seg_file_end ? seg_page_end : seg_file_end;
        page_file_start = ph->p_offset + (seg_page_start - ph->p_vaddr);
        page_file_len = copy_end - seg_page_start;
        void *dst = (void *)(uintptr_t)(as->hhdm_offset + pa + (seg_page_start - va));
        const void *src = (const uint8_t *)image + page_file_start;
        kmemcpy(dst, src, page_file_len);
      }
    }

    if (ph->p_vaddr + ph->p_memsz > high) { high = ph->p_vaddr + ph->p_memsz; }
    if (ehdr->e_phoff >= ph->p_offset &&
        ehdr->e_phoff + (uint64_t)ehdr->e_phentsize * ehdr->e_phnum <= ph->p_offset + ph->p_filesz) {
      phdr_va = ph->p_vaddr + (ehdr->e_phoff - ph->p_offset);
    }
  }

  out->entry = ehdr->e_entry;
  out->phdr = phdr_va;
  out->phent = ehdr->e_phentsize;
  out->phnum = ehdr->e_phnum;
  out->brk_base = align_up(high, PAGE_SIZE);
  as->brk_base = out->brk_base;
  as->brk_current = out->brk_base;
  return high != 0;
}
