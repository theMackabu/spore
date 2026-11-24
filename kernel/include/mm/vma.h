#pragma once

#include "fs/vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum vma_type {
  VMA_ANON,
  VMA_STACK,
  VMA_FILE,
};

struct vma {
  uint64_t start;
  uint64_t end;
  uint32_t prot;
  uint32_t flags;
  enum vma_type type;
  bool shared_may_write;
  struct vfs_node file_node;
  uint64_t file_start;
  uint64_t file_offset;
  uint64_t file_size;
};

struct vma_list {
  struct vma **chunks;
  size_t directory_pages;
  size_t count;
  size_t chunk_count;
};

void vma_list_init(struct vma_list *list);
void vma_list_destroy(struct vma_list *list);
const struct vma *vma_lookup(const struct vma_list *list, uint64_t va);
const struct vma *vma_lookup_range(const struct vma_list *list, uint64_t start, uint64_t end);
bool vma_find_gap(const struct vma_list *list, uint64_t start, uint64_t limit, uint64_t len, uint64_t *out);
bool vma_overlaps(const struct vma_list *list, uint64_t start, uint64_t end);
bool vma_insert(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, enum vma_type type);
bool vma_insert_file(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags,
                     const struct vfs_node *node, uint64_t file_start, uint64_t file_offset, uint64_t file_size, bool shared_may_write);
bool vma_remove(struct vma_list *list, uint64_t start, uint64_t end);
bool vma_protect(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot);
bool vma_extend(struct vma_list *list, uint64_t old_end, uint64_t new_end, bool extend_file);
bool vma_grow_down(struct vma_list *list, uint64_t page, uint32_t required_flags, struct vma *out);
bool vma_clone(struct vma_list *dst, const struct vma_list *src);
size_t vma_count(const struct vma_list *list);
uint64_t vma_virtual_pages(const struct vma_list *list);
size_t vma_first_ending_after(const struct vma_list *list, uint64_t addr);
const struct vma *vma_at(const struct vma_list *list, size_t index);
