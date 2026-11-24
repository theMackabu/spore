#include "handlers.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "proc/policy.h"
#include "fs/ramfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  MAP_PRIVATE = 0x02,
  MAP_SHARED = 0x01,
  MAP_FIXED = 0x10,
  MAP_ANONYMOUS = 0x20,
  MAP_GROWSDOWN = 0x0100,
  MAP_FIXED_NOREPLACE = 0x100000,
  MS_ASYNC = 1,
  MS_INVALIDATE = 2,
  MS_SYNC = 4,
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
  EPERM = 1,
  EBADF = 9,
  EACCES = 13,
  ENODEV = 19,
  O_ACCMODE = 3,
  O_RDONLY = 0,
  O_RDWR = 2,
  O_PATH = 010000000,
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

static bool find_free_mmap_range(uint64_t len, uint64_t *base_out) {
  const struct vma_list *vmas = cell_domain_vmas_const(cell_current_domain_internal());
  uint64_t start = syscall_active_as()->mmap_base;
  if (vma_find_gap(vmas, start, USER_VA_LIMIT, len, base_out)) { return true; }
  return start > USER_MMAP_BASE && vma_find_gap(vmas, USER_MMAP_BASE, USER_VA_LIMIT, len, base_out);
}

int64_t sys_brk(uint64_t requested) {
  struct user_address_space *as = syscall_active_as();
  if (requested < as->brk_base || requested > USER_VA_LIMIT || requested == 0) { return (int64_t)as->brk_current; }
  uint64_t old_end = align_up(as->brk_current, PAGE_SIZE);
  uint64_t new_end = align_up(requested, PAGE_SIZE);
  if (new_end < requested) { return (int64_t)as->brk_current; }
  if (new_end < old_end) {
    if (!cell_remove_vma(new_end, old_end)) { return (int64_t)as->brk_current; }
    as->brk_current = requested;
    return (int64_t)requested;
  }
  if (new_end > old_end) {
    if (!cell_map_add_allowed(old_end, new_end, false)) { return (int64_t)as->brk_current; }
    if (!cell_add_vma(old_end, new_end, VMM_USER_READ | VMM_USER_WRITE, 0)) { return (int64_t)as->brk_current; }
  }
  as->brk_current = requested;
  return (int64_t)requested;
}

int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off) {
  struct user_address_space *as = syscall_active_as();
  bool anon = (flags & MAP_ANONYMOUS) != 0;
  bool private = (flags & MAP_PRIVATE) != 0;
  bool shared = (flags & MAP_SHARED) != 0;
  if (!len || private == shared || (off & (PAGE_SIZE - 1))) { return -EINVAL; }
  bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
  if (fixed && (addr & (PAGE_SIZE - 1))) { return -EINVAL; }
  if (len > USER_VA_LIMIT) { return -ENOMEM; }
  uint64_t size = align_up(len, PAGE_SIZE);
  uint64_t base = align_down(addr, PAGE_SIZE);
  bool fits = base <= USER_VA_LIMIT - size;
  if (fixed && !fits) { return -ENOMEM; }
  uint64_t end = fits ? base + size : 0;
  bool automatic = !fixed && (addr == 0 || !fits || cell_vma_overlaps(base, end));
  if (automatic) {
    if (!find_free_mmap_range(size, &base)) { return -(int64_t)ENOMEM; }
    end = base + size;
  }
  if ((flags & MAP_FIXED_NOREPLACE) != 0 && cell_vma_overlaps(base, end)) { return -(int64_t)EEXIST; }
  struct vfs_node node;
  bool shared_may_write = false;
  if (!anon) {
    struct domain *domain = cell_current_domain_internal();
    if (!domain || fd >= MAX_FDS || !domain->fds[fd]) { return -EBADF; }
    struct open_file *file = domain->fds[fd];
    uint32_t access = file->flags & O_ACCMODE;
    if (file->flags & O_PATH) { return -EBADF; }
    if (access != O_RDONLY && access != O_RDWR) { return -EACCES; }
    if (shared && (prot & PROT_WRITE) && access != O_RDWR) { return -EACCES; }
    if (file->type != OPEN_VFS || file->node.device != RAMFS_DEV_NONE || file->node.is_dir ||
        (file->node.backend != VFS_EXT2 && file->node.backend != VFS_RAMFS)) { return -ENODEV; }
    if (!vfs_refresh(&file->node, &node)) { return -ENODEV; }
    if (off > UINT64_MAX - size) { return -EINVAL; }
    shared_may_write = shared && access == O_RDWR && !vfs_write_sealed(&node);
    if (shared && (prot & PROT_WRITE) && !shared_may_write) { return -EPERM; }
  }
  if (!cell_map_add_allowed(base, end, (flags & MAP_FIXED) != 0)) { return -(int64_t)ENOMEM; }
  if ((flags & MAP_FIXED) != 0 && !cell_remove_vma(base, end)) { return -(int64_t)ENOMEM; }
  if (anon) {
    enum vma_type type = (flags & MAP_GROWSDOWN) != 0 ? VMA_STACK : VMA_ANON;
    if (!cell_add_vma_typed(base, end, prot_to_vmm(prot), (uint32_t)flags, type)) { return -(int64_t)ENOMEM; }
    if (shared && type == VMA_ANON && !cell_prefault_anon_vma(base, end)) {
      (void)cell_remove_vma(base, end);
      return -(int64_t)ENOMEM;
    }
  } else {
    if (!cell_add_file_vma(base, end, prot_to_vmm(prot), (uint32_t)flags, &node, base, off, len, shared_may_write)) {
      return -(int64_t)ENOMEM;
    }
  }
  if (automatic || (!fixed && end > as->mmap_base)) { as->mmap_base = end; }
  return (int64_t)base;
}

int64_t sys_munmap(uint64_t addr, uint64_t len) {
  if (len == 0) { return -(int64_t)EINVAL; }
  uint64_t start = align_down(addr, PAGE_SIZE);
  uint64_t end = align_up(addr + len, PAGE_SIZE);
  return cell_remove_vma(start, end) ? 0 : -(int64_t)EINVAL;
}

int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
  if ((addr & (PAGE_SIZE - 1)) || (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))) { return -EINVAL; }
  if (!len) { return 0; }
  if (addr >= USER_VA_LIMIT || len > USER_VA_LIMIT - addr) { return -ENOMEM; }
  return cell_protect_vma(addr, align_up(addr + len, PAGE_SIZE), prot_to_vmm(prot));
}

int64_t sys_msync(uint64_t addr, uint64_t len, uint64_t flags) {
  if ((addr & (PAGE_SIZE - 1)) != 0 || len == 0 || (flags & ~(uint64_t)(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0 ||
      ((flags & MS_ASYNC) != 0 && (flags & MS_SYNC) != 0)) {
    return -(int64_t)EINVAL;
  }
  uint64_t end;
  if (!checked_add(addr, len, &end)) { return -(int64_t)EINVAL; }
  end = align_up(end, PAGE_SIZE);
  return cell_sync_shared_vmas(addr, end) ? 0 : -(int64_t)EINVAL;
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

static bool add_mremap_vma(uint64_t start, uint64_t end, const struct vma *old_vma, uint64_t file_start,
                           uint64_t file_size) {
  if (old_vma->type == VMA_FILE) {
    return cell_add_file_vma(start, end, old_vma->prot, old_vma->flags, &old_vma->file_node, file_start,
                             old_vma->file_offset, file_size, old_vma->shared_may_write);
  }
  return cell_add_vma_typed(start, end, old_vma->prot, old_vma->flags, old_vma->type);
}

int64_t sys_mremap(uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint64_t flags, uint64_t new_addr) {
  if ((old_addr & (PAGE_SIZE - 1)) != 0 || (flags & ~(uint64_t)(MREMAP_MAYMOVE | MREMAP_FIXED)) != 0 || new_len == 0 ||
      ((flags & MREMAP_FIXED) != 0 && (flags & MREMAP_MAYMOVE) == 0)) {
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
      if (!cell_map_add_allowed(old_end, in_place_end, false)) { return -(int64_t)ENOMEM; }
      if (!add_mremap_vma(old_end, in_place_end, &old_vma, old_vma.file_start, new_size)) { return -(int64_t)ENOMEM; }
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

  if (!cell_map_resize_allowed(old_addr, old_end, new_start, new_end, (flags & MREMAP_FIXED) != 0)) {
    return -(int64_t)ENOMEM;
  }
  if ((flags & MREMAP_FIXED) && !cell_remove_vma(new_start, new_end)) { return -ENOMEM; }
  if (cell_vma_overlaps(new_start, new_end)) { return -(int64_t)ENOMEM; }
  uint64_t file_start = new_start;
  uint64_t file_size = new_size;
  if (old_vma.type == VMA_FILE) {
    if (old_addr >= old_vma.file_start) {
      uint64_t delta = old_addr - old_vma.file_start;
      if (!checked_add(old_vma.file_offset, delta, &old_vma.file_offset)) { return -EINVAL; }
      if (!(old_vma.flags & (MAP_PRIVATE | MAP_SHARED))) {
        uint64_t left = delta < old_vma.file_size ? old_vma.file_size - delta : 0;
        if (left < file_size) { file_size = left; }
      }
    } else {
      uint64_t prefix = old_vma.file_start - old_addr;
      if (prefix > new_size) { prefix = new_size; }
      file_start += prefix;
      file_size = old_vma.file_size < new_size - prefix ? old_vma.file_size : new_size - prefix;
    }
  }
  if (!add_mremap_vma(new_start, new_end, &old_vma, file_start, file_size)) { return -ENOMEM; }
  uint64_t bytes = old_size < new_size ? old_size : new_size;
  struct user_address_space *as = syscall_active_as();
  if (!vmm_move_range(as, old_addr, new_start, bytes)) {
    (void)cell_remove_vma(new_start, new_end);
    return -ENOMEM;
  }
  if (!cell_remove_vma(old_addr, old_end)) {
    (void)vmm_move_range(as, new_start, old_addr, bytes);
    (void)cell_remove_vma(new_start, new_end);
    return -ENOMEM;
  }
  if ((flags & MREMAP_FIXED) == 0) { syscall_active_as()->mmap_base = new_end; }
  return (int64_t)new_start;
}
