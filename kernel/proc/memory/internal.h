#pragma once

#include "proc/memory.h"

constexpr unsigned MAP_SHARED = 1;

static inline uint32_t mapping_flags(const struct vma *vma) {
  return vma->prot | ((vma->flags & MAP_SHARED) ? VMM_MAP_SHARED : 0);
}

bool cell_flush_shared_file_vmas(struct process_mm *mm, uint64_t start, uint64_t end);
