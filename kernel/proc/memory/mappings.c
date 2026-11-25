#include "fs/vfs.h"
#include "internal.h"
#include "mm/pmm.h"
#include "proc/domain.h"

enum { EIO = 5, ENOMEM = 12, EACCES = 13, EINVAL = 22 };

static bool vma_is_shared_writable_file(const struct vma *vma) {
  return vma != NULL && vma->type == VMA_FILE && (vma->flags & MAP_SHARED) != 0 && (vma->prot & VMM_USER_WRITE) != 0;
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

bool cell_flush_shared_file_vmas(struct process_mm *mm, uint64_t start, uint64_t end) {
  if (mm == NULL) { return true; }
  for (size_t i = vma_first_ending_after(&mm->vmas, start); i < vma_count(&mm->vmas); ++i) {
    const struct vma *vma = vma_at(&mm->vmas, i);
    if (vma->start >= end) { break; }
    if (!flush_shared_file_vma_range(&mm->as, vma, start, end)) { return false; }
  }
  return true;
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
                       uint64_t file_start, uint64_t file_offset, uint64_t file_size, bool shared_may_write) {
  struct domain *domain = cell_current_domain_internal();
  struct vma_list *vmas = cell_domain_vmas(domain);
  return vmas != NULL &&
         vma_insert_file(vmas, start, end, prot, flags, node, file_start, file_offset, file_size, shared_may_write);
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
  if (domain->mm != NULL && !cell_flush_shared_file_vmas(domain->mm, start, end)) { return false; }
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
    if ((prot & VMM_USER_EXEC) && vma->type == VMA_FILE && vfs_mount_flag(vma->file_node.access, MOUNT_NOEXEC)) {
      return -EACCES;
    }
    if ((prot & VMM_USER_WRITE) && vma->type == VMA_FILE && (vma->flags & MAP_SHARED) && !vma->shared_may_write) {
      return -EACCES;
    }
    if (!(prot & VMM_USER_WRITE) && !flush_shared_file_vma_range(as, vma, start, end)) { return -EIO; }
    covered = vma->end;
  }
  if (!vma_protect(vmas, start, end, prot)) { return -ENOMEM; }
  vmm_protect_range(as, start, end, prot);
  return 0;
}
