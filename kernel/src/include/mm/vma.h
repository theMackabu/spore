#pragma once

#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  VMA_MAX_CHUNKS = 64,
  VMA_CHUNK_BYTES = 4096,
};

enum vma_type {
  VMA_ANON,
  VMA_STACK,
  VMA_FILE,
};

struct vma {
  bool used;
  uint64_t start;
  uint64_t end;
  uint32_t prot;
  uint32_t flags;
  enum vma_type type;
  struct vfs_node file_node;
  uint64_t file_start;
  uint64_t file_offset;
  uint64_t file_size;
};

struct vma_list {
  struct vma *chunks[VMA_MAX_CHUNKS];
  uint64_t chunk_pa[VMA_MAX_CHUNKS];
  size_t chunk_count;
};

void vma_set_hhdm_offset(uint64_t hhdm_offset);
void vma_list_init(struct vma_list *list);
void vma_list_destroy(struct vma_list *list);
const struct vma *vma_lookup(const struct vma_list *list, uint64_t va);
const struct vma *vma_lookup_range(const struct vma_list *list, uint64_t start, uint64_t end);
bool vma_overlaps(const struct vma_list *list, uint64_t start, uint64_t end);
bool vma_insert(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, enum vma_type type);
bool vma_insert_file(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags,
                     const struct vfs_node *node, uint64_t file_start, uint64_t file_offset, uint64_t file_size);
bool vma_remove(struct vma_list *list, uint64_t start, uint64_t end);
bool vma_protect(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot);
bool vma_clone(struct vma_list *dst, const struct vma_list *src);
size_t vma_count(const struct vma_list *list);
uint64_t vma_virtual_pages(const struct vma_list *list);
size_t vma_capacity(const struct vma_list *list);
const struct vma *vma_at(const struct vma_list *list, size_t index);
