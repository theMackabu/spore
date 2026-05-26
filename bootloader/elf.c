#include "bootloader.h"

#define ELF_MAGIC 0x464c457f
#define EM_AARCH64 183
#define PT_LOAD 1

struct elf64_ehdr {
  uint8_t e_ident[16];
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

EFI_STATUS load_kernel(struct loaded_file *kernel_file, uint64_t *kernel_phys_base, uint64_t *kernel_virt_base,
                       uint64_t *kernel_span, uint64_t *entry) {
  EFI_STATUS status = read_file(u"\\boot\\kernel.elf", kernel_file);
  if (EFI_ERROR(status)) { return status; }
  if (kernel_file->size < sizeof(struct elf64_ehdr)) { return EFI_LOAD_ERROR; }
  struct elf64_ehdr *eh = kernel_file->data;
  if (*(uint32_t *)eh->e_ident != ELF_MAGIC || eh->e_machine != EM_AARCH64 ||
      eh->e_phoff + (uint64_t)eh->e_phentsize * eh->e_phnum > kernel_file->size) {
    return EFI_LOAD_ERROR;
  }

  uint64_t min_vaddr = UINT64_MAX;
  uint64_t max_vaddr = 0;
  for (uint16_t i = 0; i < eh->e_phnum; ++i) {
    struct elf64_phdr *ph =
      (struct elf64_phdr *)((uint8_t *)kernel_file->data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
    if (ph->p_type != PT_LOAD) { continue; }
    uint64_t start = align_down(ph->p_vaddr, PAGE_SIZE);
    uint64_t end = align_up(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
    if (start < min_vaddr) { min_vaddr = start; }
    if (end > max_vaddr) { max_vaddr = end; }
  }
  if (min_vaddr == UINT64_MAX || max_vaddr <= min_vaddr) { return EFI_LOAD_ERROR; }

  void *load = NULL;
  status = alloc_pages(pages_for(max_vaddr - min_vaddr), &load);
  if (EFI_ERROR(status)) { return status; }
  for (uint16_t i = 0; i < eh->e_phnum; ++i) {
    struct elf64_phdr *ph =
      (struct elf64_phdr *)((uint8_t *)kernel_file->data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
    if (ph->p_type != PT_LOAD) { continue; }
    if (ph->p_offset + ph->p_filesz > kernel_file->size || ph->p_filesz > ph->p_memsz) { return EFI_LOAD_ERROR; }
    uint64_t dst_off = ph->p_vaddr - min_vaddr;
    memcpy((uint8_t *)load + dst_off, (uint8_t *)kernel_file->data + ph->p_offset, ph->p_filesz);
    memset((uint8_t *)load + dst_off + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
  }
  *kernel_phys_base = (uint64_t)(uintptr_t)load;
  *kernel_virt_base = min_vaddr;
  *kernel_span = max_vaddr - min_vaddr;
  *entry = eh->e_entry;
  return EFI_SUCCESS;
}
