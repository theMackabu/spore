#include "internal.h"
#include "proc/domain.h"
#include "lib/mem.h"
#include "mm/pool.h"
#include "proc/limits.h"

static struct object_pool process_mms;

struct user_address_space *cell_current_as(void) {
  struct domain *domain = cell_current_domain_internal();
  return cell_domain_as(domain);
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
  (void)cell_flush_shared_file_vmas(mm, 0, UINT64_MAX);
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
