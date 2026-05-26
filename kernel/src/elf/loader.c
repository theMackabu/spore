#include "elf/loader.h"

#include "mem.h"

enum {
  EI_NIDENT = 16,
  ET_EXEC = 2,
  ET_DYN = 3,
  EM_AARCH64 = 183,
  PT_INTERP = 3,
  PT_LOAD = 1,
  PF_X = 1,
  PF_W = 2,
  PF_R = 4,
  PAGE_SIZE = 4096,
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

static bool bounds_ok(uint64_t off, uint64_t len, uint64_t image_size) {
  return off <= image_size && len <= image_size - off;
}

static bool read_exact(const struct elf_reader *reader, uint64_t off, void *dst, size_t len) {
  return reader != NULL && reader->read_at != NULL && bounds_ok(off, len, reader->size) &&
         reader->read_at(reader->ctx, off, dst, len);
}

static uint32_t phdr_to_vmm_flags(uint32_t flags) {
  uint32_t out = VMM_USER_READ;
  if ((flags & PF_W) != 0) { out |= VMM_USER_WRITE; }
  if ((flags & PF_X) != 0) { out |= VMM_USER_EXEC; }
  return out;
}

static bool valid_ehdr(const struct elf64_ehdr *ehdr, uint64_t image_size) {
  const unsigned char magic[4] = {0x7f, 'E', 'L', 'F'};
  return image_size >= sizeof(*ehdr) && kmemcmp(ehdr->e_ident, magic, sizeof(magic)) == 0 && ehdr->e_ident[4] == 2 &&
         ehdr->e_ident[5] == 1 && (ehdr->e_type == ET_EXEC || ehdr->e_type == ET_DYN) &&
         ehdr->e_machine == EM_AARCH64 && ehdr->e_phentsize == sizeof(struct elf64_phdr) &&
         bounds_ok(ehdr->e_phoff, (uint64_t)ehdr->e_phentsize * ehdr->e_phnum, image_size);
}

static bool read_ehdr(const struct elf_reader *reader, struct elf64_ehdr *ehdr) {
  return read_exact(reader, 0, ehdr, sizeof(*ehdr)) && valid_ehdr(ehdr, reader->size);
}

bool elf_find_interp_aarch64(const struct elf_reader *reader, char *out, size_t out_cap) {
  if (out_cap == 0) { return false; }
  struct elf64_ehdr ehdr;
  if (!read_ehdr(reader, &ehdr)) { return false; }

  for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
    struct elf64_phdr ph;
    uint64_t ph_off = ehdr.e_phoff + (uint64_t)i * sizeof(ph);
    if (!read_exact(reader, ph_off, &ph, sizeof(ph))) { return false; }
    if (ph.p_type != PT_INTERP) { continue; }
    if (ph.p_filesz == 0 || ph.p_filesz >= out_cap || !bounds_ok(ph.p_offset, ph.p_filesz, reader->size)) {
      return false;
    }
    if (!read_exact(reader, ph.p_offset, out, (size_t)ph.p_filesz)) { return false; }
    if (out[ph.p_filesz - 1] != '\0') { return false; }
    return true;
  }
  return false;
}

bool elf_load_aarch64(struct user_address_space *as, struct vma_list *vmas, const struct elf_reader *reader,
                      uint64_t load_base, struct loaded_elf *out) {
  struct elf64_ehdr ehdr;
  if (as == NULL || vmas == NULL || reader == NULL || reader->node == NULL || !read_ehdr(reader, &ehdr)) {
    return false;
  }

  uint64_t high = 0;
  uint64_t phdr_va = 0;
  uint64_t base = ehdr.e_type == ET_DYN ? load_base : 0;

  for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
    struct elf64_phdr ph;
    uint64_t ph_off = ehdr.e_phoff + (uint64_t)i * sizeof(ph);
    if (!read_exact(reader, ph_off, &ph, sizeof(ph))) { return false; }
    if (ph.p_type != PT_LOAD) { continue; }
    if (ph.p_memsz < ph.p_filesz || !bounds_ok(ph.p_offset, ph.p_filesz, reader->size) ||
        (ph.p_align > PAGE_SIZE && (ph.p_vaddr % ph.p_align) != (ph.p_offset % ph.p_align))) {
      return false;
    }

    uint64_t seg_vaddr = base + ph.p_vaddr;
    uint64_t start = align_down(seg_vaddr, PAGE_SIZE);
    uint64_t end = align_up(seg_vaddr + ph.p_memsz, PAGE_SIZE);
    uint32_t flags = phdr_to_vmm_flags(ph.p_flags);
    if (!vma_insert_file(vmas, start, end, flags, 0, reader->node, seg_vaddr, ph.p_offset, ph.p_filesz)) {
      return false;
    }

    if (seg_vaddr + ph.p_memsz > high) { high = seg_vaddr + ph.p_memsz; }
    if (ehdr.e_phoff >= ph.p_offset &&
        ehdr.e_phoff + (uint64_t)ehdr.e_phentsize * ehdr.e_phnum <= ph.p_offset + ph.p_filesz) {
      phdr_va = seg_vaddr + (ehdr.e_phoff - ph.p_offset);
    }
  }

  out->entry = base + ehdr.e_entry;
  out->runtime_entry = out->entry;
  out->load_base = base;
  out->at_base = 0;
  out->phdr = phdr_va;
  out->phent = ehdr.e_phentsize;
  out->phnum = ehdr.e_phnum;
  out->brk_base = align_up(high, PAGE_SIZE);
  as->brk_base = out->brk_base;
  as->brk_current = out->brk_base;
  return high != 0;
}
