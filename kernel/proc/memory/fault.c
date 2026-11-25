#include "internal.h"
#include "proc/domain.h"
#include "fs/vfs.h"
#include "mm/pmm.h"
#include "proc/thread.h"

constexpr unsigned MAP_GROWSDOWN = 0x0100;

static bool access_allowed(const struct vma *vma, enum vmm_access access) {
  switch (access) {
  case VMM_ACCESS_READ:
    return (vma->prot & VMM_USER_READ) != 0;
  case VMM_ACCESS_WRITE:
    return (vma->prot & VMM_USER_WRITE) != 0;
  case VMM_ACCESS_EXEC:
    return (vma->prot & VMM_USER_EXEC) != 0;
  }
  return false;
}

static bool fault_file_page(struct domain *domain, const struct vma *vma, uint64_t page) {
  struct user_address_space *as = cell_domain_as(domain);
  if (as == NULL) { return false; }
  if (vmm_user_range_accessible(as, page, 1, VMM_ACCESS_READ) &&
      ((vma->prot & VMM_USER_EXEC) == 0 || vmm_user_range_accessible(as, page, 1, VMM_ACCESS_EXEC))) {
    return true;
  }
  struct vfs_node current;
  if (!vfs_refresh(&vma->file_node, &current)) { return false; }
  uint64_t page_end = page + PAGE_SIZE;
  uint64_t file_end = vma->file_start + vma->file_size;
  if (page >= vma->file_start && (page_end <= file_end || ((vma->flags & MAP_SHARED) && page < file_end))) {
    uint64_t read_off = vma->file_offset + (page - vma->file_start);
    if ((read_off & (PAGE_SIZE - 1)) != 0) { goto private_copy; }
    uint64_t pa = 0;
    if (current.backend == VFS_RAMFS && !(vma->flags & MAP_SHARED)) {
      pa = ramfs_boot_page(&current.ramfs, read_off);
      if (pa && !pmm_retain_boot_page(pa)) { pa = 0; }
    }
    if (!pa && !vfs_retain_page_cached(&current, read_off, (vma->prot & VMM_USER_EXEC) != 0, &pa)) { return false; }
    bool mapped = (vma->flags & MAP_SHARED) ? vmm_map_page(as, page, pa, mapping_flags(vma) | VMM_MAP_FILE_CACHE)
                                             : vmm_map_page_cow(as, page, pa, vma->prot);
    if (!mapped) {
      pmm_free_page(pa);
      return false;
    }
    return true;
  }

private_copy:
  uint64_t pa = pmm_alloc_zero_page();
  if (pa == 0) { return false; }
  uint8_t *dst = (uint8_t *)(uintptr_t)(as->hhdm_offset + pa);
  uint64_t copy_start = page > vma->file_start ? page : vma->file_start;
  uint64_t copy_end = page_end < file_end ? page_end : file_end;
  if (copy_start < copy_end) {
    uint64_t read_off = vma->file_offset + (copy_start - vma->file_start);
    uint64_t len = copy_end - copy_start;
    if (vfs_read(&current, read_off, dst + (copy_start - page), len) != len) {
      pmm_free_page(pa);
      return false;
    }
  }
  if (!vmm_map_page(as, page, pa, mapping_flags(vma))) {
    pmm_free_page(pa);
    return false;
  }
  return true;
}

static void note_fault(struct domain *domain, bool major) {
  if (domain == NULL) { return; }
  struct thread *thread = cell_current_thread_internal();
  if (thread && thread->domain == domain) {
    if (major) { ++thread->major_faults; }
    else { ++thread->minor_faults; }
  }
  if (major) {
    ++domain->major_faults;
  } else {
    ++domain->minor_faults;
  }
}

bool cell_domain_ensure_user_range(struct domain *domain, uint64_t va, size_t len, enum vmm_access access) {
  if (domain == NULL || len == 0) { return true; }
  struct user_address_space *as = cell_domain_as(domain);
  struct vma_list *vmas = cell_domain_vmas(domain);
  if (as == NULL || vmas == NULL) { return false; }
  if (vmm_user_buffer_accessible(as, va, len, access)) { return true; }
  uint64_t end = va + len - 1;
  if (end < va) { return false; }
  uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
  uint64_t last = end & ~(uint64_t)(PAGE_SIZE - 1);
  for (;;) {
    if (!vmm_is_mapped(as, page)) {
      struct vma grown;
      const struct vma *vma = vma_lookup(vmas, page);
      if (vma == NULL && vma_grow_down(vmas, page, MAP_GROWSDOWN, &grown)) { vma = &grown; }
      if (!vma || (vma->type != VMA_ANON && vma->type != VMA_STACK && vma->type != VMA_FILE) || !access_allowed(vma, access)) { return false; }
      if (vma->type == VMA_FILE) {
        if (!fault_file_page(domain, vma, page)) { return false; }
        note_fault(domain, true);
      } else if (!vmm_alloc_page(as, page, mapping_flags(vma))) {
        return false;
      } else {
        note_fault(domain, false);
      }
    }
    if (access == VMM_ACCESS_WRITE && !vmm_user_buffer_accessible(as, page, 1, VMM_ACCESS_WRITE)) {
      if (!vmm_handle_write_fault(as, page)) { return false; }
      note_fault(domain, false);
    }
    if (!vmm_user_buffer_accessible(as, page, 1, access)) { return false; }
    if (page == last) { return true; }
    page += PAGE_SIZE;
  }
}

bool cell_handle_write_fault(uint64_t far) {
  struct domain *domain = cell_current_domain_internal();
  struct user_address_space *as = cell_domain_as(domain);
  if (domain == NULL || as == NULL || !vmm_handle_write_fault(as, far)) { return false; }
  note_fault(domain, false);
  return true;
}

bool cell_handle_translation_fault(uint64_t far, enum vmm_access access) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return false; }
  struct user_address_space *as = cell_domain_as(domain);
  struct vma_list *vmas = cell_domain_vmas(domain);
  if (as == NULL || vmas == NULL) { return false; }
  uint64_t va = far & ~(uint64_t)(PAGE_SIZE - 1);
  struct vma grown;
  const struct vma *vma = vma_lookup(vmas, va);
  if (vma == NULL && vma_grow_down(vmas, va, MAP_GROWSDOWN, &grown)) { vma = &grown; }
  if (vma == NULL || !access_allowed(vma, access)) { return false; }
  if (vmm_user_range_accessible(as, va, 1, access)) { return true; }
  if (vma->type == VMA_FILE) {
    if (!fault_file_page(domain, vma, va)) { return false; }
    note_fault(domain, true);
    return true;
  }
  if (vma->type != VMA_ANON && vma->type != VMA_STACK) { return false; }
  if (!vmm_alloc_page(as, va, mapping_flags(vma))) { return false; }
  note_fault(domain, false);
  return true;
}

bool cell_handle_permission_fault(uint64_t far, enum vmm_access access) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return false; }
  struct user_address_space *as = cell_domain_as(domain);
  struct vma_list *vmas = cell_domain_vmas(domain);
  if (as == NULL || vmas == NULL) { return false; }
  uint64_t va = far & ~(uint64_t)(PAGE_SIZE - 1);
  const struct vma *vma = vma_lookup(vmas, va);
  if (vma == NULL || !access_allowed(vma, access) || !vmm_is_mapped(as, va)) { return false; }
  vmm_protect_range(as, va, va + PAGE_SIZE, vma->prot);
  return vmm_user_range_accessible(as, va, 1, access);
}

bool cell_ensure_user_range(uint64_t va, size_t len, enum vmm_access access) {
  return cell_domain_ensure_user_range(cell_current_domain_internal(), va, len, access);
}

bool cell_prefault_anon_vma(uint64_t start, uint64_t end) {
  struct domain *domain = cell_current_domain_internal();
  struct user_address_space *as = cell_domain_as(domain);
  struct vma_list *vmas = cell_domain_vmas(domain);
  if (as == NULL || vmas == NULL || start >= end) { return false; }
  const struct vma *vma = vma_lookup_range(vmas, start, end);
  if (vma == NULL || vma->type != VMA_ANON) { return false; }
  for (uint64_t page = start; page < end; page += PAGE_SIZE) {
    if (vmm_is_mapped(as, page)) { continue; }
    if (!vmm_alloc_page(as, page, mapping_flags(vma))) {
      vmm_unmap_range(as, start, page);
      return false;
    }
    note_fault(domain, false);
  }
  return true;
}
