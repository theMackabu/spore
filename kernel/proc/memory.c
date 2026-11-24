#include "proc/memory.h"

#include "lib/mem.h"
#include "mm/pmm.h"
#include "mm/pool.h"
#include "proc/domain.h"
#include "proc/limits.h"
#include "proc/thread.h"
#include "fs/vfs.h"

enum {
  MAP_SHARED = 0x01,
  MAP_GROWSDOWN = 0x0100,
  EIO = 5,
  ENOMEM = 12,
  EACCES = 13,
  EINVAL = 22,
};

static struct object_pool process_mms;

struct user_address_space *cell_current_as(void) {
  struct domain *domain = cell_current_domain_internal();
  return cell_domain_as(domain);
}

static bool vma_is_shared_writable_file(const struct vma *vma);
static bool flush_shared_file_vmas(struct process_mm *mm, uint64_t start, uint64_t end);

static uint32_t mapping_flags(const struct vma *vma) {
  return vma->prot | ((vma->flags & MAP_SHARED) ? VMM_MAP_SHARED : 0);
}

bool cell_mm_reset(size_t capacity) {
  return capacity <= MAX_DOMAINS + MAX_SNAPSHOTS && pool_init(&process_mms, sizeof(struct process_mm), capacity);
}

static struct process_mm *alloc_mm(void) {
  struct process_mm *mm = pool_alloc(&process_mms);
  if (mm != NULL) { mm->refcount = 1; }
  return mm;
}

struct process_mm *cell_mm_from_owned(struct user_address_space *as, struct vma_list *vmas) {
  if (as == NULL || vmas == NULL) { return NULL; }
  struct process_mm *mm = alloc_mm();
  if (mm == NULL) { return NULL; }
  mm->as = *as;
  mm->vmas = *vmas;
  kmemset(as, 0, sizeof(*as));
  vma_list_init(vmas);
  return mm;
}

struct process_mm *cell_mm_clone_cow(struct process_mm *src) {
  if (src == NULL) { return NULL; }
  struct process_mm *mm = alloc_mm();
  if (mm == NULL) { return NULL; }
  if (!vmm_clone(&mm->as, &src->as, VMM_FORK)) {
    pool_free(&process_mms, mm);
    return NULL;
  }
  if (!vma_clone(&mm->vmas, &src->vmas)) {
    vmm_destroy(&mm->as);
    pool_free(&process_mms, mm);
    return NULL;
  }
  return mm;
}

struct process_mm *cell_mm_retain(struct process_mm *mm) {
  if (mm != NULL) { ++mm->refcount; }
  return mm;
}

void cell_mm_release(struct process_mm *mm) {
  if (mm == NULL || mm->refcount == 0) { return; }
  --mm->refcount;
  if (mm->refcount != 0) { return; }
  (void)flush_shared_file_vmas(mm, 0, UINT64_MAX);
  vmm_destroy(&mm->as);
  vma_list_destroy(&mm->vmas);
  pool_free(&process_mms, mm);
}

bool cell_domain_set_mm(struct domain *domain, struct process_mm *mm) {
  if (domain == NULL || mm == NULL) { return false; }
  if (domain->mm != NULL) {
    uint64_t peak = domain->mm->as.peak_resident_pages;
    if (peak > domain->previous_peak_rss) { domain->previous_peak_rss = peak; }
    cell_mm_release(domain->mm);
  }
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

static bool vma_is_shared_writable_file(const struct vma *vma) {
  return vma != NULL && vma->type == VMA_FILE && (vma->flags & MAP_SHARED) != 0 &&
         (vma->prot & VMM_USER_WRITE) != 0;
}

struct file_writeback {
  const struct vma *vma;
  const struct vfs_node *node;
  uint64_t file_end;
};

static bool writeback_page(void *ctx, uint64_t page, uint64_t pa) {
  const struct file_writeback *wb = ctx;
  uint64_t offset = wb->vma->file_offset + (page - wb->vma->file_start);
  uint64_t bytes = wb->file_end - page;
  if (bytes > PAGE_SIZE) { bytes = PAGE_SIZE; }
  return vfs_writeback(wb->node, offset, pmm_phys_to_virt(pa), bytes) == (int64_t)bytes;
}

static bool flush_shared_file_vma_range(struct user_address_space *as, const struct vma *vma, uint64_t start,
                                        uint64_t end) {
  if (vma->type != VMA_FILE || !(vma->flags & MAP_SHARED)) { return true; }
  if (start < vma->start) { start = vma->start; }
  if (end > vma->end) { end = vma->end; }
  uint64_t first = vma->file_offset + (start - vma->file_start);
  uint64_t limit = vma->file_offset + (end - vma->file_start);
  if (!vfs_sync_mapped_range(&vma->file_node, first, limit)) { return false; }
  if (!as->detached_file_pages || !vma_is_shared_writable_file(vma)) { return true; }
  struct vfs_node current;
  if (!vfs_refresh(&vma->file_node, &current)) { return false; }
  if (current.size <= vma->file_offset) { return true; }
  uint64_t bytes = current.size - vma->file_offset;
  if (bytes > vma->file_size) { bytes = vma->file_size; }
  struct file_writeback wb = {.vma = vma, .node = &current, .file_end = vma->file_start + bytes};
  if (end > wb.file_end) { end = wb.file_end; }
  return vmm_visit_detached(as, start, end, writeback_page, &wb);
}

static bool flush_shared_file_vmas(struct process_mm *mm, uint64_t start, uint64_t end) {
  if (mm == NULL) { return true; }
  for (size_t i = vma_first_ending_after(&mm->vmas, start); i < vma_count(&mm->vmas); ++i) {
    const struct vma *vma = vma_at(&mm->vmas, i);
    if (vma->start >= end) { break; }
    if (!flush_shared_file_vma_range(&mm->as, vma, start, end)) { return false; }
  }
  return true;
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
    if (!vfs_retain_page_cached(&current, read_off, &pa)) { return false; }
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
      if (vma == NULL && cell_grow_down_vma(page, MAP_GROWSDOWN, &grown)) { vma = &grown; }
      if (!domain_access_allowed(vma, access)) { return false; }
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
  if (vma == NULL && cell_grow_down_vma(va, MAP_GROWSDOWN, &grown)) { vma = &grown; }
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

bool cell_add_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags) {
  return cell_add_vma_typed(start, end, prot, flags, VMA_ANON);
}

bool cell_add_vma_typed(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, enum vma_type type) {
  struct domain *domain = cell_current_domain_internal();
  struct vma_list *vmas = cell_domain_vmas(domain);
  return vmas != NULL && vma_insert(vmas, start, end, prot, flags, type);
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

bool cell_grow_down_vma(uint64_t page, uint32_t required_flags, struct vma *out) {
  struct domain *domain = cell_current_domain_internal();
  struct vma_list *vmas = cell_domain_vmas(domain);
  return vmas != NULL && vma_grow_down(vmas, page, required_flags, out);
}

bool cell_add_file_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, const struct vfs_node *node,
                       uint64_t file_start, uint64_t file_offset, uint64_t file_size, bool shared_may_write) {
  struct domain *domain = cell_current_domain_internal();
  struct vma_list *vmas = cell_domain_vmas(domain);
  return vmas != NULL && vma_insert_file(vmas, start, end, prot, flags, node, file_start, file_offset, file_size, shared_may_write);
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
  if (domain->mm != NULL && !flush_shared_file_vmas(domain->mm, start, end)) { return false; }
  if (!vma_remove(vmas, start, end)) { return false; }
  vmm_unmap_range(as, start, end);
  return true;
}

int cell_sync_shared_vmas(uint64_t start, uint64_t end, bool durable) {
  struct domain *domain = cell_current_domain_internal();
  if (!domain || !domain->mm) { return -ENOMEM; }
  const struct vma_list *vmas = &domain->mm->vmas;
  uint64_t covered = start;
  bool disk = false;
  for (size_t i = vma_first_ending_after(vmas, start); covered < end; ++i) {
    const struct vma *vma = vma_at(vmas, i);
    if (!vma || vma->start > covered) { return -ENOMEM; }
    if (!flush_shared_file_vma_range(&domain->mm->as, vma, start, end)) { return -EIO; }
    if (vma->type == VMA_FILE && (vma->flags & MAP_SHARED) && vma->file_node.backend == VFS_EXT2) { disk = true; }
    covered = vma->end;
  }
  return durable && disk && !vfs_sync_storage() ? -EIO : 0;
}

int cell_discard_vma_pages(uint64_t start, uint64_t end, bool lazy_free) {
  struct domain *domain = cell_current_domain_internal();
  struct vma_list *vmas = cell_domain_vmas(domain);
  struct user_address_space *as = cell_domain_as(domain);
  if (!vmas || !as) { return -ENOMEM; }
  for (size_t i = vma_first_ending_after(vmas, start); start < end; ++i) {
    const struct vma *vma = vma_at(vmas, i);
    if (!vma || vma->start > start) { return -ENOMEM; }
    bool shared = (vma->flags & MAP_SHARED) != 0;
    if (lazy_free && (shared || vma->type == VMA_FILE)) { return -EINVAL; }
    uint64_t next = end < vma->end ? end : vma->end;
    if (!shared || vma->type == VMA_FILE) {
      if (!flush_shared_file_vma_range(as, vma, start, next)) { return -EIO; }
      vmm_unmap_range(as, start, next);
    }
    start = next;
  }
  return 0;
}

int cell_protect_vma(uint64_t start, uint64_t end, uint32_t prot) {
  struct domain *domain = cell_current_domain_internal();
  struct vma_list *vmas = cell_domain_vmas(domain);
  struct user_address_space *as = cell_domain_as(domain);
  if (!vmas || !as) { return -ENOMEM; }
  uint64_t covered = start;
  for (size_t i = vma_first_ending_after(vmas, start); covered < end; ++i) {
    const struct vma *vma = vma_at(vmas, i);
    if (!vma || vma->start > covered) { return -ENOMEM; }
    if ((prot & VMM_USER_WRITE) && vma->type == VMA_FILE && (vma->flags & MAP_SHARED) &&
        !vma->shared_may_write) { return -EACCES; }
    if (!(prot & VMM_USER_WRITE) && !flush_shared_file_vma_range(as, vma, start, end)) { return -EIO; }
    covered = vma->end;
  }
  if (!vma_protect(vmas, start, end, prot)) { return -ENOMEM; }
  vmm_protect_range(as, start, end, prot);
  return 0;
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
  out->resident_pages = as->resident_pages;
  out->minor_faults = domain->minor_faults;
  out->major_faults = domain->major_faults;
  return true;
}
