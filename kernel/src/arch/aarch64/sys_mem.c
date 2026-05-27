#include "arch/aarch64/syscall_handlers.h"

#include "cell.h"
#include "mm/pmm.h"
#include "ramfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  MAP_PRIVATE = 0x02,
  MAP_FIXED = 0x10,
  MAP_ANONYMOUS = 0x20,
  MAP_FIXED_NOREPLACE = 0x100000,
  MREMAP_MAYMOVE = 1,
  MREMAP_FIXED = 2,
  PROT_READ = 0x1,
  PROT_WRITE = 0x2,
  PROT_EXEC = 0x4,
  MADV_DONTNEED = 4,
  MADV_FREE = 8,
  ENOMEM = 12,
  EFAULT = 14,
  EEXIST = 17,
  EINVAL = 22,
};

static uint64_t align_down(uint64_t value, uint64_t align) {
  return value & ~(align - 1);
}

static uint64_t align_up(uint64_t value, uint64_t align) {
  return (value + align - 1) & ~(align - 1);
}

static bool checked_add(uint64_t a, uint64_t b, uint64_t *out) {
  *out = a + b;
  return *out >= a;
}

static uint32_t prot_to_vmm(uint64_t prot) {
  uint32_t out = 0;
  if ((prot & PROT_READ) != 0) { out |= VMM_USER_READ; }
  if ((prot & PROT_WRITE) != 0) { out |= VMM_USER_WRITE; }
  if ((prot & PROT_EXEC) != 0) { out |= VMM_USER_EXEC; }
  return out;
}

int64_t sys_brk(uint64_t requested) {
  struct user_address_space *as = syscall_active_as();
  if (requested == 0 || requested < as->brk_base) { return (int64_t)as->brk_current; }
  uint64_t old_end = align_up(as->brk_current, PAGE_SIZE);
  uint64_t new_end = align_up(requested, PAGE_SIZE);
  if (new_end < requested) { return (int64_t)as->brk_current; }
  if (new_end < old_end) {
    if (!cell_remove_vma(new_end, old_end)) { return (int64_t)as->brk_current; }
    as->brk_current = requested;
    return (int64_t)requested;
  }
  if (new_end > old_end) {
    if (!cell_add_vma(old_end, new_end, VMM_USER_READ | VMM_USER_WRITE, 0)) {
      return (int64_t)as->brk_current;
    }
  }
  as->brk_current = requested;
  return (int64_t)requested;
}

int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off) {
  struct user_address_space *as = syscall_active_as();
  bool anon = (flags & MAP_ANONYMOUS) != 0;
  if (len == 0 || (flags & MAP_PRIVATE) == 0 || (!anon && (int64_t)fd < 0)) { return -(int64_t)EINVAL; }
  uint64_t base = addr != 0 ? align_down(addr, PAGE_SIZE) : as->mmap_base;
  uint64_t raw_end;
  if (!checked_add(base, len, &raw_end)) { return -(int64_t)EINVAL; }
  uint64_t end = align_up(raw_end, PAGE_SIZE);
  if (end < raw_end) { return -(int64_t)EINVAL; }
  if (!cell_mmap_allowed((end - base) / PAGE_SIZE)) { return -(int64_t)ENOMEM; }
  if ((flags & MAP_FIXED_NOREPLACE) != 0 && cell_vma_overlaps(base, end)) { return -(int64_t)EEXIST; }
  if ((flags & MAP_FIXED) != 0) { (void)cell_remove_vma(base, end); }
  if (anon) {
    if (!cell_add_vma(base, end, prot_to_vmm(prot), (uint32_t)flags)) { return -(int64_t)EINVAL; }
  } else {
    struct vfs_node node;
    if (!cell_fd_stat((int)fd, &node) || node.is_dir || node.device != RAMFS_DEV_NONE) { return -(int64_t)EINVAL; }
    if (!cell_add_file_vma(base, end, prot_to_vmm(prot), (uint32_t)flags, &node, base, off, len)) {
      return -(int64_t)EINVAL;
    }
  }
  if (addr == 0) { as->mmap_base = end; }
  return (int64_t)base;
}

int64_t sys_munmap(uint64_t addr, uint64_t len) {
  if (len == 0) { return -(int64_t)EINVAL; }
  uint64_t start = align_down(addr, PAGE_SIZE);
  uint64_t end = align_up(addr + len, PAGE_SIZE);
  return cell_remove_vma(start, end) ? 0 : -(int64_t)EINVAL;
}

int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
  if (len == 0) { return 0; }
  uint64_t start = align_down(addr, PAGE_SIZE);
  uint64_t end = align_up(addr + len, PAGE_SIZE);
  uint32_t vmm_prot = prot_to_vmm(prot);
  if (cell_protect_vma(start, end, vmm_prot)) { return 0; }
  for (uint64_t va = start; va < end; va += PAGE_SIZE) {
    if (!vmm_is_mapped(syscall_active_as(), va)) { return -(int64_t)EINVAL; }
  }
  vmm_protect_range(syscall_active_as(), start, end, vmm_prot);
  return 0;
}

int64_t sys_madvise(uint64_t addr, uint64_t len, uint64_t advice) {
  if (advice == MADV_DONTNEED || advice == MADV_FREE) {
    vmm_unmap_range(syscall_active_as(), align_down(addr, PAGE_SIZE), align_up(addr + len, PAGE_SIZE));
  }
  return 0;
}

static bool ranges_overlap(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end) {
  return a_start < b_end && b_start < a_end;
}

static bool find_free_mmap_range(uint64_t len, uint64_t *base_out) {
  struct user_address_space *as = syscall_active_as();
  uint64_t base = align_up(as->mmap_base, PAGE_SIZE);
  for (size_t attempts = 0; attempts < 4096; ++attempts) {
    uint64_t end;
    if (!checked_add(base, len, &end)) { return false; }
    if (!cell_vma_overlaps(base, end)) {
      *base_out = base;
      as->mmap_base = end;
      return true;
    }
    base = align_up(end + PAGE_SIZE, PAGE_SIZE);
  }
  return false;
}

static bool move_mapped_pages(uint64_t old_start, uint64_t old_len, uint64_t new_start) {
  for (uint64_t off = 0; off < old_len; off += PAGE_SIZE) {
    if (!vmm_move_page(syscall_active_as(), old_start + off, new_start + off)) { return false; }
  }
  return true;
}

int64_t sys_mremap(uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint64_t flags, uint64_t new_addr) {
  if ((old_addr & (PAGE_SIZE - 1)) != 0 || (flags & ~(uint64_t)(MREMAP_MAYMOVE | MREMAP_FIXED)) != 0 ||
      new_len == 0 || ((flags & MREMAP_FIXED) != 0 && (flags & MREMAP_MAYMOVE) == 0)) {
    return -(int64_t)EINVAL;
  }

  uint64_t old_size = align_up(old_len, PAGE_SIZE);
  uint64_t new_size = align_up(new_len, PAGE_SIZE);
  uint64_t old_end;
  if (old_len == 0 || old_size < old_len || new_size < new_len || !checked_add(old_addr, old_size, &old_end)) {
    return -(int64_t)EINVAL;
  }

  struct vma old_vma;
  if (!cell_vma_lookup_range(old_addr, old_end, &old_vma)) { return -(int64_t)EFAULT; }

  if (old_vma.type == VMA_STACK && new_size > old_size && (flags & (MREMAP_MAYMOVE | MREMAP_FIXED)) == 0) {
    return -(int64_t)ENOMEM;
  }

  if (new_size == old_size && (flags & MREMAP_FIXED) == 0) { return (int64_t)old_addr; }

  if (new_size < old_size && (flags & MREMAP_FIXED) == 0) {
    uint64_t tail = old_addr + new_size;
    return cell_remove_vma(tail, old_end) ? (int64_t)old_addr : -(int64_t)ENOMEM;
  }

  if ((flags & MREMAP_FIXED) == 0) {
    uint64_t in_place_end;
    if (checked_add(old_addr, new_size, &in_place_end) && !cell_vma_overlaps(old_end, in_place_end)) {
      if (!cell_add_vma_typed(old_end, in_place_end, old_vma.prot, old_vma.flags, old_vma.type)) {
        return -(int64_t)ENOMEM;
      }
      return (int64_t)old_addr;
    }
    if ((flags & MREMAP_MAYMOVE) == 0) { return -(int64_t)ENOMEM; }
  }

  uint64_t new_start;
  if ((flags & MREMAP_FIXED) != 0) {
    if ((new_addr & (PAGE_SIZE - 1)) != 0) { return -(int64_t)EINVAL; }
    new_start = new_addr;
  } else if (!find_free_mmap_range(new_size, &new_start)) {
    return -(int64_t)ENOMEM;
  }
  uint64_t new_end;
  if (!checked_add(new_start, new_size, &new_end)) { return -(int64_t)EINVAL; }
  if (ranges_overlap(old_addr, old_end, new_start, new_end)) { return -(int64_t)EINVAL; }

  if ((flags & MREMAP_FIXED) != 0) { (void)cell_remove_vma(new_start, new_end); }
  if (cell_vma_overlaps(new_start, new_end)) { return -(int64_t)ENOMEM; }
  if (!move_mapped_pages(old_addr, old_size < new_size ? old_size : new_size, new_start)) { return -(int64_t)ENOMEM; }
  (void)cell_remove_vma(old_addr, old_end);
  if (!cell_add_vma_typed(new_start, new_end, old_vma.prot, old_vma.flags, old_vma.type)) { return -(int64_t)ENOMEM; }
  return (int64_t)new_start;
}
