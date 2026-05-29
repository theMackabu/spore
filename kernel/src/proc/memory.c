#include "proc/memory.h"

#include "mem.h"
#include "mm/pmm.h"
#include "proc/thread.h"
#include "vfs.h"

static struct process_mm process_mms[MAX_DOMAINS + MAX_SNAPSHOTS];

void cell_mm_reset(void) {
  kmemset(process_mms, 0, sizeof(process_mms));
}

static struct process_mm *alloc_mm(void) {
  for (size_t i = 0; i < sizeof(process_mms) / sizeof(process_mms[0]); ++i) {
    if (!process_mms[i].used) {
      kmemset(&process_mms[i], 0, sizeof(process_mms[i]));
      process_mms[i].used = true;
      process_mms[i].refcount = 1;
      return &process_mms[i];
    }
  }
  return NULL;
}

struct process_mm *cell_mm_from_owned(struct user_address_space *as, struct vma_list *vmas) {
  struct process_mm *mm = alloc_mm();
  if (mm == NULL || as == NULL || vmas == NULL) {
    if (mm != NULL) { mm->used = false; }
    return NULL;
  }
  mm->as = *as;
  mm->vmas = *vmas;
  kmemset(as, 0, sizeof(*as));
  vma_list_init(vmas);
  return mm;
}

struct process_mm *cell_mm_clone_cow(struct process_mm *src) {
  if (src == NULL || !src->used) { return NULL; }
  struct process_mm *mm = alloc_mm();
  if (mm == NULL) { return NULL; }
  if (!vmm_clone_cow(&mm->as, &src->as, 0)) {
    mm->used = false;
    return NULL;
  }
  if (!vma_clone(&mm->vmas, &src->vmas)) {
    vmm_destroy(&mm->as);
    mm->used = false;
    return NULL;
  }
  return mm;
}

struct process_mm *cell_mm_retain(struct process_mm *mm) {
  if (mm != NULL && mm->used) { ++mm->refcount; }
  return mm;
}

void cell_mm_release(struct process_mm *mm) {
  if (mm == NULL || !mm->used || mm->refcount == 0) { return; }
  --mm->refcount;
  if (mm->refcount != 0) { return; }
  vmm_destroy(&mm->as);
  vma_list_destroy(&mm->vmas);
  mm->used = false;
}

bool cell_domain_set_mm(struct domain *domain, struct process_mm *mm) {
  if (domain == NULL || mm == NULL || !mm->used) { return false; }
  if (domain->mm != NULL) { cell_mm_release(domain->mm); }
  domain->mm = mm;
  return true;
}

struct user_address_space *cell_domain_as(struct domain *domain) {
  return domain == NULL || domain->mm == NULL ? NULL : &domain->mm->as;
}

const struct user_address_space *cell_domain_as_const(const struct domain *domain) {
  return domain == NULL || domain->mm == NULL ? NULL : &domain->mm->as;
}

struct vma_list *cell_domain_vmas(struct domain *domain) {
  return domain == NULL || domain->mm == NULL ? NULL : &domain->mm->vmas;
}

const struct vma_list *cell_domain_vmas_const(const struct domain *domain) {
  return domain == NULL || domain->mm == NULL ? NULL : &domain->mm->vmas;
}

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
  struct user_address_space *as = cell_domain_as(domain);
  if (as == NULL) { return false; }
  uint64_t pa = pmm_alloc_zero_page();
  if (pa == 0) { return false; }
  uint8_t *dst = (uint8_t *)(uintptr_t)(as->hhdm_offset + pa);
  uint64_t page_end = page + PAGE_SIZE;
  uint64_t file_end = vma->file_start + vma->file_size;
  uint64_t copy_start = page > vma->file_start ? page : vma->file_start;
  uint64_t copy_end = page_end < file_end ? page_end : file_end;
  if (copy_start < copy_end) {
    uint8_t cached_page[PAGE_SIZE];
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
  if (!vmm_map_page(as, page, pa, vma->prot)) {
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
  struct user_address_space *as = cell_domain_as(domain);
  struct vma_list *vmas = cell_domain_vmas(domain);
  if (as == NULL || vmas == NULL) { return false; }
  uint64_t end = va + len - 1;
  if (end < va) { return false; }
  uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
  uint64_t last = end & ~(uint64_t)(PAGE_SIZE - 1);
  for (;;) {
    if (!vmm_is_mapped(as, page)) {
      const struct vma *vma = vma_lookup(vmas, page);
      if (!domain_access_allowed(vma, access)) { return false; }
      if (vma->type == VMA_FILE) {
        if (!fault_file_page(domain, vma, page)) { return false; }
        note_fault(domain, true);
      } else if (!vmm_alloc_page(as, page, vma->prot)) {
        return false;
      } else {
        note_fault(domain, false);
      }
    }
    if (access == VMM_ACCESS_WRITE && !vmm_user_range_accessible(as, page, 1, VMM_ACCESS_WRITE)) {
      if (!vmm_handle_cow_fault(as, page)) { return false; }
      note_fault(domain, false);
    }
    if (!vmm_user_range_accessible(as, page, 1, access)) { return false; }
    if (page == last) { return true; }
    page += PAGE_SIZE;
  }
}

bool cell_handle_cow_fault(uint64_t far) {
  struct domain *domain = cell_current_domain_internal();
  struct user_address_space *as = cell_domain_as(domain);
  if (domain == NULL || as == NULL || !vmm_handle_cow_fault(as, far)) { return false; }
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
  const struct vma *vma = vma_lookup(vmas, va);
  if (vma == NULL || !access_allowed(vma, access)) { return false; }
  if (vma->type == VMA_FILE) {
    if (!fault_file_page(domain, vma, va)) { return false; }
    note_fault(domain, true);
    return true;
  }
  if (vma->type != VMA_ANON && vma->type != VMA_STACK) { return false; }
  if (!vmm_alloc_page(as, va, vma->prot)) { return false; }
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

bool cell_add_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags) {
  return cell_add_vma_typed(start, end, prot, flags, VMA_ANON);
}

bool cell_add_vma_typed(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, enum vma_type type) {
  struct domain *domain = cell_current_domain_internal();
  struct vma_list *vmas = cell_domain_vmas(domain);
  return vmas != NULL && vma_insert(vmas, start, end, prot, flags, type);
}

bool cell_add_file_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, const struct vfs_node *node,
                       uint64_t file_start, uint64_t file_offset, uint64_t file_size) {
  struct domain *domain = cell_current_domain_internal();
  struct vma_list *vmas = cell_domain_vmas(domain);
  return vmas != NULL && vma_insert_file(vmas, start, end, prot, flags, node, file_start, file_offset, file_size);
}

bool cell_vma_overlaps(uint64_t start, uint64_t end) {
  struct domain *domain = cell_current_domain_internal();
  struct vma_list *vmas = cell_domain_vmas(domain);
  return vmas == NULL || vma_overlaps(vmas, start, end);
}

bool cell_vma_lookup_range(uint64_t start, uint64_t end, struct vma *out) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return false; }
  struct vma_list *vmas = cell_domain_vmas(domain);
  if (vmas == NULL) { return false; }
  const struct vma *vma = vma_lookup_range(vmas, start, end);
  if (vma == NULL) { return false; }
  if (out != NULL) { *out = *vma; }
  return true;
}

bool cell_remove_vma(uint64_t start, uint64_t end) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return false; }
  struct user_address_space *as = cell_domain_as(domain);
  struct vma_list *vmas = cell_domain_vmas(domain);
  if (as == NULL || vmas == NULL) { return false; }
  vmm_unmap_range(as, start, end);
  return vma_remove(vmas, start, end);
}

bool cell_protect_vma(uint64_t start, uint64_t end, uint32_t prot) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return false; }
  struct user_address_space *as = cell_domain_as(domain);
  struct vma_list *vmas = cell_domain_vmas(domain);
  if (as == NULL || vmas == NULL) { return false; }
  if (!vma_protect(vmas, start, end, prot)) { return false; }
  vmm_protect_range(as, start, end, prot);
  return true;
}

size_t cell_resident_pages(uint64_t start, uint64_t end) {
  struct domain *domain = cell_current_domain_internal();
  struct user_address_space *as = cell_domain_as(domain);
  return as == NULL ? 0 : vmm_mapped_pages_in_range(as, start, end);
}

bool cell_memory_accounting(const struct domain *domain, struct cell_memory_accounting *out) {
  if (domain == NULL || out == NULL || !domain->used) { return false; }
  const struct user_address_space *as = cell_domain_as_const(domain);
  const struct vma_list *vmas = cell_domain_vmas_const(domain);
  if (as == NULL || vmas == NULL) { return false; }
  out->virtual_pages = vma_virtual_pages(vmas);
  out->resident_pages = 0;
  for (size_t i = 0; i < vma_capacity(vmas); ++i) {
    const struct vma *vma = vma_at(vmas, i);
    if (vma->used) { out->resident_pages += vmm_mapped_pages_in_range(as, vma->start, vma->end); }
  }
  out->minor_faults = domain->minor_faults;
  out->major_faults = domain->major_faults;
  return true;
}
