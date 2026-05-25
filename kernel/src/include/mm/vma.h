#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  MAX_VMAS = 128,
};

enum vma_type {
  VMA_ANON,
};

struct vma {
  bool used;
  uint64_t start;
  uint64_t end;
  uint32_t prot;
  uint32_t flags;
  enum vma_type type;
};

struct vma_list {
  struct vma entries[MAX_VMAS];
};

void vma_list_init(struct vma_list *list);
const struct vma *vma_lookup(const struct vma_list *list, uint64_t va);
bool vma_overlaps(const struct vma_list *list, uint64_t start, uint64_t end);
bool vma_insert(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, enum vma_type type);
bool vma_remove(struct vma_list *list, uint64_t start, uint64_t end);
bool vma_protect(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot);
bool vma_clone(struct vma_list *dst, const struct vma_list *src);
size_t vma_count(const struct vma_list *list);
