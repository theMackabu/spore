#include "mm/vma.h"

#include "mem.h"
#include "mm/pmm.h"

#if __STDC_HOSTED__
#include <stdlib.h>
#else
#endif

#define VMAS_PER_CHUNK (VMA_CHUNK_BYTES / sizeof(struct vma))

static uint64_t vma_hhdm_offset;

static bool overlaps(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end) {
  return a_start < b_end && b_start < a_end;
}

static bool compatible(const struct vma *a, const struct vma *b) {
  if (a->prot != b->prot || a->flags != b->flags || a->type != b->type) { return false; }
  if (a->type != VMA_FILE) { return true; }
  return a->file_node.backend == b->file_node.backend && a->file_node.ino == b->file_node.ino &&
         a->file_node.dev_id == b->file_node.dev_id && a->file_start == b->file_start &&
         a->file_offset == b->file_offset && a->file_size == b->file_size;
}

void vma_set_hhdm_offset(uint64_t hhdm_offset) {
  vma_hhdm_offset = hhdm_offset;
}

static struct vma *chunk_alloc(uint64_t *pa_out) {
#if __STDC_HOSTED__
  struct vma *chunk = malloc(VMA_CHUNK_BYTES);
  if (chunk == NULL) { return NULL; }
  kmemset(chunk, 0, VMA_CHUNK_BYTES);
  *pa_out = 0;
  return chunk;
#else
  if (vma_hhdm_offset == 0) { return NULL; }
  uint64_t pa = pmm_alloc_zero_page();
  if (pa == 0) { return NULL; }
  *pa_out = pa;
  return (struct vma *)(uintptr_t)(vma_hhdm_offset + pa);
#endif
}

static void chunk_free(struct vma *chunk, uint64_t pa) {
  if (chunk == NULL) { return; }
#if __STDC_HOSTED__
  (void)pa;
  free(chunk);
#else
  if (pa != 0) { pmm_free_page(pa); }
#endif
}

static bool ensure_chunk(struct vma_list *list) {
  if (list->chunk_count >= VMA_MAX_CHUNKS) { return false; }
  uint64_t pa = 0;
  struct vma *chunk = chunk_alloc(&pa);
  if (chunk == NULL) { return false; }
  list->chunks[list->chunk_count] = chunk;
  list->chunk_pa[list->chunk_count] = pa;
  ++list->chunk_count;
  return true;
}

const struct vma *vma_at(const struct vma_list *list, size_t index) {
  if (list == NULL || index >= vma_capacity(list)) { return NULL; }
  return &list->chunks[index / VMAS_PER_CHUNK][index % VMAS_PER_CHUNK];
}

static struct vma *vma_at_mut(struct vma_list *list, size_t index) {
  return (struct vma *)(uintptr_t)vma_at(list, index);
}

static struct vma *alloc_slot(struct vma_list *list) {
  for (;;) {
    for (size_t i = 0; i < vma_capacity(list); ++i) {
      struct vma *slot = vma_at_mut(list, i);
      if (!slot->used) { return slot; }
    }
    if (!ensure_chunk(list)) { return NULL; }
  }
}

static bool add_raw_vma(struct vma_list *list, const struct vma *src) {
  if (src == NULL || src->start >= src->end) { return false; }
  struct vma *slot = alloc_slot(list);
  if (slot == NULL) { return false; }
  *slot = *src;
  slot->used = true;
  return true;
}

static bool add_raw(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags,
                    enum vma_type type) {
  if (start >= end) { return false; }
  struct vma *slot = alloc_slot(list);
  if (slot == NULL) { return false; }
  kmemset(slot, 0, sizeof(*slot));
  slot->used = true;
  slot->start = start;
  slot->end = end;
  slot->prot = prot;
  slot->flags = flags;
  slot->type = type;
  return true;
}

static void merge_adjacent(struct vma_list *list) {
  for (size_t i = 0; i < vma_capacity(list); ++i) {
    struct vma *a = vma_at_mut(list, i);
    if (!a->used) { continue; }
    for (size_t j = 0; j < vma_capacity(list); ++j) {
      struct vma *b = vma_at_mut(list, j);
      if (i == j || !b->used) { continue; }
      if (a->end == b->start && compatible(a, b)) {
        a->end = b->end;
        b->used = false;
        j = 0;
      } else if (b->end == a->start && compatible(a, b)) {
        a->start = b->start;
        b->used = false;
        j = 0;
      }
    }
  }
}

void vma_list_init(struct vma_list *list) {
  kmemset(list, 0, sizeof(*list));
}

void vma_list_destroy(struct vma_list *list) {
  if (list == NULL) { return; }
  for (size_t i = 0; i < list->chunk_count; ++i) {
    chunk_free(list->chunks[i], list->chunk_pa[i]);
  }
  vma_list_init(list);
}

size_t vma_capacity(const struct vma_list *list) {
  return list == NULL ? 0 : list->chunk_count * VMAS_PER_CHUNK;
}

const struct vma *vma_lookup(const struct vma_list *list, uint64_t va) {
  for (size_t i = 0; i < vma_capacity(list); ++i) {
    const struct vma *vma = vma_at(list, i);
    if (vma->used && va >= vma->start && va < vma->end) { return vma; }
  }
  return NULL;
}

const struct vma *vma_lookup_range(const struct vma_list *list, uint64_t start, uint64_t end) {
  if (start >= end) { return NULL; }
  for (size_t i = 0; i < vma_capacity(list); ++i) {
    const struct vma *vma = vma_at(list, i);
    if (vma->used && start >= vma->start && end <= vma->end) { return vma; }
  }
  return NULL;
}

bool vma_overlaps(const struct vma_list *list, uint64_t start, uint64_t end) {
  if (start >= end) { return true; }
  for (size_t i = 0; i < vma_capacity(list); ++i) {
    const struct vma *vma = vma_at(list, i);
    if (vma->used && overlaps(start, end, vma->start, vma->end)) { return true; }
  }
  return false;
}

bool vma_insert(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags,
                enum vma_type type) {
  if (start >= end) { return false; }
  if (vma_overlaps(list, start, end)) { return false; }
  if (!add_raw(list, start, end, prot, flags, type)) { return false; }
  merge_adjacent(list);
  return true;
}

bool vma_insert_file(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags,
                     const struct vfs_node *node, uint64_t file_start, uint64_t file_offset, uint64_t file_size) {
  if (start >= end || node == NULL || node->is_dir || node->device != RAMFS_DEV_NONE) { return false; }
  if (vma_overlaps(list, start, end)) { return false; }
  struct vma *slot = alloc_slot(list);
  if (slot == NULL) { return false; }
  kmemset(slot, 0, sizeof(*slot));
  slot->used = true;
  slot->start = start;
  slot->end = end;
  slot->prot = prot;
  slot->flags = flags;
  slot->type = VMA_FILE;
  slot->file_node = *node;
  slot->file_start = file_start;
  slot->file_offset = file_offset;
  slot->file_size = file_size;
  merge_adjacent(list);
  return true;
}

bool vma_remove(struct vma_list *list, uint64_t start, uint64_t end) {
  if (start >= end) { return false; }

  struct vma_list next;
  vma_list_init(&next);
  for (size_t i = 0; i < vma_capacity(list); ++i) {
    const struct vma *vma = vma_at(list, i);
    if (!vma->used) { continue; }
    if (!overlaps(start, end, vma->start, vma->end)) {
      if (!add_raw_vma(&next, vma)) {
        vma_list_destroy(&next);
        return false;
      }
      continue;
    }
    struct vma part = *vma;
    if (vma->start < start) {
      part.end = start;
      if (!add_raw_vma(&next, &part)) {
        vma_list_destroy(&next);
        return false;
      }
    }
    if (end < vma->end) {
      part = *vma;
      part.start = end;
      if (!add_raw_vma(&next, &part)) {
        vma_list_destroy(&next);
        return false;
      }
    }
  }
  merge_adjacent(&next);
  vma_list_destroy(list);
  *list = next;
  return true;
}

bool vma_protect(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot) {
  if (start >= end) { return false; }

  struct vma_list next;
  vma_list_init(&next);
  for (size_t i = 0; i < vma_capacity(list); ++i) {
    const struct vma *vma = vma_at(list, i);
    if (!vma->used) { continue; }
    if (!overlaps(start, end, vma->start, vma->end)) {
      if (!add_raw_vma(&next, vma)) {
        vma_list_destroy(&next);
        return false;
      }
      continue;
    }
    struct vma part = *vma;
    if (vma->start < start) {
      part.end = start;
      if (!add_raw_vma(&next, &part)) {
        vma_list_destroy(&next);
        return false;
      }
    }
    uint64_t mid_start = vma->start > start ? vma->start : start;
    uint64_t mid_end = vma->end < end ? vma->end : end;
    part = *vma;
    part.start = mid_start;
    part.end = mid_end;
    part.prot = prot;
    if (!add_raw_vma(&next, &part)) {
      vma_list_destroy(&next);
      return false;
    }
    if (end < vma->end) {
      part = *vma;
      part.start = end;
      if (!add_raw_vma(&next, &part)) {
        vma_list_destroy(&next);
        return false;
      }
    }
  }
  merge_adjacent(&next);
  vma_list_destroy(list);
  *list = next;
  return true;
}

bool vma_clone(struct vma_list *dst, const struct vma_list *src) {
  vma_list_init(dst);
  for (size_t i = 0; i < vma_capacity(src); ++i) {
    const struct vma *vma = vma_at(src, i);
    if (!vma->used) { continue; }
    if (!add_raw_vma(dst, vma)) {
      vma_list_destroy(dst);
      return false;
    }
  }
  return true;
}

size_t vma_count(const struct vma_list *list) {
  size_t count = 0;
  for (size_t i = 0; i < vma_capacity(list); ++i) {
    const struct vma *vma = vma_at(list, i);
    if (vma->used) { ++count; }
  }
  return count;
}

uint64_t vma_virtual_pages(const struct vma_list *list) {
  uint64_t pages = 0;
  for (size_t i = 0; i < vma_capacity(list); ++i) {
    const struct vma *vma = vma_at(list, i);
    if (!vma->used) { continue; }
    pages += (vma->end - vma->start + PAGE_SIZE - 1) / PAGE_SIZE;
  }
  return pages;
}
