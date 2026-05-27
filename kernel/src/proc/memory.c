#include "proc/memory.h"

#include "mem.h"
#include "mm/pmm.h"
#include "proc/thread.h"
#include "vfs.h"

static bool domain_access_allowed(const struct vma *vma, enum vmm_access access) {
  if (vma == NULL || (vma->type != VMA_ANON && vma->type != VMA_STACK && vma->type != VMA_FILE)) { return false; }
  if (access == VMM_ACCESS_READ) { return (vma->prot & VMM_USER_READ) != 0; }
  if (access == VMM_ACCESS_WRITE) { return (vma->prot & VMM_USER_WRITE) != 0; }
  if (access == VMM_ACCESS_EXEC) { return (vma->prot & VMM_USER_EXEC) != 0; }
  return false;
}

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
  uint64_t pa = pmm_alloc_zero_page();
  if (pa == 0) { return false; }
  uint8_t *dst = (uint8_t *)(uintptr_t)(domain->as.hhdm_offset + pa);
  uint64_t page_end = page + PAGE_SIZE;
  uint64_t file_end = vma->file_start + vma->file_size;
  uint64_t copy_start = page > vma->file_start ? page : vma->file_start;
  uint64_t copy_end = page_end < file_end ? page_end : file_end;
  if (copy_start < copy_end) {
    static uint8_t cached_page[PAGE_SIZE];
    uint64_t copied = 0;
    while (copied < copy_end - copy_start) {
      uint64_t read_off = vma->file_offset + (copy_start - vma->file_start) + copied;
      uint64_t file_page = read_off & ~(uint64_t)(PAGE_SIZE - 1);
      uint64_t page_off = read_off - file_page;
      uint64_t chunk = PAGE_SIZE - page_off;
      if (chunk > copy_end - copy_start - copied) { chunk = copy_end - copy_start - copied; }
      if (!vfs_read_page_cached(&vma->file_node, file_page, cached_page)) {
        pmm_free_page(pa);
        return false;
      }
      kmemcpy(dst + (copy_start - page) + copied, cached_page + page_off, chunk);
      copied += chunk;
    }
  }
  if (!vmm_map_page(&domain->as, page, pa, vma->prot)) {
    pmm_free_page(pa);
    return false;
  }
  return true;
}

static void note_fault(struct domain *domain, bool major) {
  if (domain == NULL) { return; }
  if (major) {
    ++domain->major_faults;
  } else {
    ++domain->minor_faults;
  }
}

bool cell_domain_ensure_user_range(struct domain *domain, uint64_t va, size_t len, enum vmm_access access) {
  if (domain == NULL || len == 0) { return true; }
  uint64_t end = va + len - 1;
  if (end < va) { return false; }
  uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
  uint64_t last = end & ~(uint64_t)(PAGE_SIZE - 1);
  for (;;) {
    if (!vmm_is_mapped(&domain->as, page)) {
      const struct vma *vma = vma_lookup(&domain->vmas, page);
      if (!domain_access_allowed(vma, access)) { return false; }
      if (vma->type == VMA_FILE) {
        if (!fault_file_page(domain, vma, page)) { return false; }
        note_fault(domain, true);
      } else if (!vmm_alloc_page(&domain->as, page, vma->prot)) {
        return false;
      } else {
        note_fault(domain, false);
      }
    }
    if (access == VMM_ACCESS_WRITE && !vmm_user_range_accessible(&domain->as, page, 1, VMM_ACCESS_WRITE)) {
      if (!vmm_handle_cow_fault(&domain->as, page)) { return false; }
      note_fault(domain, false);
    }
    if (!vmm_user_range_accessible(&domain->as, page, 1, access)) { return false; }
    if (page == last) { return true; }
    page += PAGE_SIZE;
  }
}

bool cell_handle_cow_fault(uint64_t far) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || !vmm_handle_cow_fault(&domain->as, far)) { return false; }
  note_fault(domain, false);
  return true;
}

bool cell_handle_translation_fault(uint64_t far, enum vmm_access access) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return false; }
  uint64_t va = far & ~(uint64_t)(PAGE_SIZE - 1);
  const struct vma *vma = vma_lookup(&domain->vmas, va);
  if (vma == NULL || !access_allowed(vma, access)) { return false; }
  if (vma->type == VMA_FILE) {
    if (!fault_file_page(domain, vma, va)) { return false; }
    note_fault(domain, true);
    return true;
  }
  if (vma->type != VMA_ANON && vma->type != VMA_STACK) { return false; }
  if (!vmm_alloc_page(&domain->as, va, vma->prot)) { return false; }
  note_fault(domain, false);
  return true;
}

bool cell_ensure_user_range(uint64_t va, size_t len, enum vmm_access access) {
  return cell_domain_ensure_user_range(cell_current_domain_internal(), va, len, access);
}

bool cell_add_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags) {
  return cell_add_vma_typed(start, end, prot, flags, VMA_ANON);
}

bool cell_add_vma_typed(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, enum vma_type type) {
  struct domain *domain = cell_current_domain_internal();
  return domain != NULL && vma_insert(&domain->vmas, start, end, prot, flags, type);
}

bool cell_add_file_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, const struct vfs_node *node,
                       uint64_t file_start, uint64_t file_offset, uint64_t file_size) {
  struct domain *domain = cell_current_domain_internal();
  return domain != NULL && vma_insert_file(&domain->vmas, start, end, prot, flags, node, file_start, file_offset,
                                           file_size);
}

bool cell_vma_overlaps(uint64_t start, uint64_t end) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL || vma_overlaps(&domain->vmas, start, end);
}

bool cell_vma_lookup_range(uint64_t start, uint64_t end, struct vma *out) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return false; }
  const struct vma *vma = vma_lookup_range(&domain->vmas, start, end);
  if (vma == NULL) { return false; }
  if (out != NULL) { *out = *vma; }
  return true;
}

bool cell_remove_vma(uint64_t start, uint64_t end) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return false; }
  vmm_unmap_range(&domain->as, start, end);
  return vma_remove(&domain->vmas, start, end);
}

bool cell_protect_vma(uint64_t start, uint64_t end, uint32_t prot) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return false; }
  if (!vma_protect(&domain->vmas, start, end, prot)) { return false; }
  vmm_protect_range(&domain->as, start, end, prot);
  return true;
}

size_t cell_resident_pages(uint64_t start, uint64_t end) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? 0 : vmm_mapped_pages_in_range(&domain->as, start, end);
}

bool cell_memory_accounting(const struct domain *domain, struct cell_memory_accounting *out) {
  if (domain == NULL || out == NULL || !domain->used) { return false; }
  out->virtual_pages = vma_virtual_pages(&domain->vmas);
  out->resident_pages = 0;
  for (size_t i = 0; i < vma_capacity(&domain->vmas); ++i) {
    const struct vma *vma = vma_at(&domain->vmas, i);
    if (vma->used) { out->resident_pages += vmm_mapped_pages_in_range(&domain->as, vma->start, vma->end); }
  }
  out->minor_faults = domain->minor_faults;
  out->major_faults = domain->major_faults;
  return true;
}
