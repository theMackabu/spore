#pragma once

#include "mm/vma.h"
#include "mm/vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct domain;

struct process_mm {
  uint16_t refcount;
  struct user_address_space as;
  struct vma_list vmas;
};

struct cell_memory_accounting {
  uint64_t virtual_pages;
  uint64_t resident_pages;
  uint64_t minor_faults;
  uint64_t major_faults;
};

bool cell_domain_ensure_user_range(struct domain *domain, uint64_t va, size_t len, enum vmm_access access);
struct user_address_space *cell_domain_as(struct domain *domain);
const struct user_address_space *cell_domain_as_const(const struct domain *domain);
struct vma_list *cell_domain_vmas(struct domain *domain);
const struct vma_list *cell_domain_vmas_const(const struct domain *domain);
bool cell_mm_reset(size_t capacity);
struct process_mm *cell_mm_from_owned(struct user_address_space *as, struct vma_list *vmas);
struct process_mm *cell_mm_clone_cow(struct process_mm *src);
struct process_mm *cell_mm_retain(struct process_mm *mm);
void cell_mm_release(struct process_mm *mm);
bool cell_domain_set_mm(struct domain *domain, struct process_mm *mm);
bool cell_handle_cow_fault(uint64_t far);
bool cell_handle_translation_fault(uint64_t far, enum vmm_access access);
bool cell_handle_permission_fault(uint64_t far, enum vmm_access access);
bool cell_ensure_user_range(uint64_t va, size_t len, enum vmm_access access);
bool cell_vma_overlaps(uint64_t start, uint64_t end);
bool cell_vma_lookup_range(uint64_t start, uint64_t end, struct vma *out);
bool cell_add_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags);
bool cell_add_vma_typed(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, enum vma_type type);
bool cell_prefault_anon_vma(uint64_t start, uint64_t end);
bool cell_grow_down_vma(uint64_t page, uint32_t required_flags, struct vma *out);
bool cell_add_file_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, const struct vfs_node *node,
                       uint64_t file_start, uint64_t file_offset, uint64_t file_size);
bool cell_remove_vma(uint64_t start, uint64_t end);
bool cell_sync_shared_vmas(uint64_t start, uint64_t end);
bool cell_protect_vma(uint64_t start, uint64_t end, uint32_t prot);
size_t cell_resident_pages(uint64_t start, uint64_t end);
bool cell_memory_accounting(const struct domain *domain, struct cell_memory_accounting *out);

struct user_address_space *cell_current_as(void);
