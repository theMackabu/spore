#include "mm/vma.h"

#include "lib/mem.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#if __STDC_HOSTED__
#include <stdlib.h>
#endif

constexpr size_t VMAS_PER_CHUNK = VMA_CHUNK_BYTES / sizeof(struct vma);

static uint64_t vma_hhdm_offset;

static bool compatible(const struct vma *a, const struct vma *b) {
  if (a->prot != b->prot || a->flags != b->flags || a->type != b->type || a->shared_may_write != b->shared_may_write) { return false; }
  if (a->type != VMA_FILE) { return true; }
  return a->file_node.backend == b->file_node.backend && a->file_node.ino == b->file_node.ino &&
         a->file_node.dev_id == b->file_node.dev_id && a->file_start == b->file_start &&
         a->file_offset == b->file_offset && a->file_size == b->file_size;
}

void vma_set_hhdm_offset(uint64_t hhdm_offset) {
  vma_hhdm_offset = hhdm_offset;
}

static void *alloc_pages(size_t pages) {
#if __STDC_HOSTED__
  return calloc(pages, VMA_CHUNK_BYTES);
#else
  if (vma_hhdm_offset == 0) { return NULL; }
  if (pages == 1) {
    uint64_t pa = pmm_alloc_zero_page();
    return pa ? (void *)(uintptr_t)(vma_hhdm_offset + pa) : NULL;
  }
  uint64_t pa = pmm_alloc_contiguous_pages(pages);
  if (!pa) { return NULL; }
  void *ptr = (void *)(uintptr_t)(vma_hhdm_offset + pa);
  kmemset(ptr, 0, pages * PAGE_SIZE);
  return ptr;
#endif
}

static void free_pages(void *ptr, size_t pages) {
#if __STDC_HOSTED__
  (void)pages;
  free(ptr);
#else
  uint64_t pa = (uintptr_t)ptr - vma_hhdm_offset;
  for (size_t i = 0; i < pages; ++i) {
    pmm_free_page(pa + i * PAGE_SIZE);
  }
#endif
}

static void trim_chunks(struct vma_list *list) {
  size_t needed = list->count / VMAS_PER_CHUNK + (list->count % VMAS_PER_CHUNK != 0);
  while (list->chunk_count > needed) {
    free_pages(list->chunks[--list->chunk_count], 1);
  }
  if (needed == 0) {
    free_pages(list->chunks, list->directory_pages);
    vma_list_init(list);
  }
}

static bool reserve_slots(struct vma_list *list, size_t count) {
  size_t needed = count / VMAS_PER_CHUNK + (count % VMAS_PER_CHUNK != 0);
  size_t pointers_per_page = VMA_CHUNK_BYTES / sizeof(*list->chunks);
  if (needed > list->directory_pages * pointers_per_page) {
    size_t pages = list->directory_pages ? list->directory_pages : 1;
    while (needed > pages * pointers_per_page) {
      if (pages > SIZE_MAX / VMA_CHUNK_BYTES / 2) { return false; }
      pages *= 2;
    }
    struct vma **chunks = alloc_pages(pages);
    if (!chunks) { return false; }
    if (list->chunks) {
      kmemcpy(chunks, list->chunks, list->chunk_count * sizeof(*chunks));
      free_pages(list->chunks, list->directory_pages);
    }
    list->chunks = chunks;
    list->directory_pages = pages;
  }
  while (list->chunk_count < needed) {
    struct vma *chunk = alloc_pages(1);
    if (!chunk) {
      trim_chunks(list);
      return false;
    }
    list->chunks[list->chunk_count++] = chunk;
  }
  return true;
}

static struct vma *slot_at(const struct vma_list *list, size_t i) {
  return &list->chunks[i / VMAS_PER_CHUNK][i % VMAS_PER_CHUNK];
}

const struct vma *vma_at(const struct vma_list *list, size_t i) {
  return list && i < list->count ? slot_at(list, i) : NULL;
}

size_t vma_first_ending_after(const struct vma_list *list, uint64_t addr) {
  size_t lo = 0, hi = list->count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (slot_at(list, mid)->end <= addr) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

static bool retain_mapping(const struct vma *vma) {
  if (vma->type != VMA_FILE) { return true; }
  if (!vfs_retain_node(&vma->file_node)) { return false; }
  if (vma->shared_may_write) {
    vfs_note_shared_writable_mapping(&vma->file_node, true);
  }
  return true;
}

static void release_mapping(const struct vma *vma) {
  if (vma->type != VMA_FILE) { return; }
  if (vma->shared_may_write) {
    vfs_note_shared_writable_mapping(&vma->file_node, false);
  }
  vfs_release_node(&vma->file_node);
}

static void erase_slots(struct vma_list *list, size_t first, size_t end) {
  if (first == end) { return; }
  for (size_t i = first; i < end; ++i) {
    release_mapping(slot_at(list, i));
  }
  size_t remaining = list->count - (end - first);
  for (size_t i = end; i < list->count; ++i) {
    *slot_at(list, first++) = *slot_at(list, i);
  }
  list->count = remaining;
  trim_chunks(list);
}

static bool insert_slot(struct vma_list *list, size_t i, const struct vma *vma) {
  if (!retain_mapping(vma)) { return false; }
  for (size_t j = list->count; j > i; --j) {
    *slot_at(list, j) = *slot_at(list, j - 1);
  }
  *slot_at(list, i) = *vma;
  ++list->count;
  return true;
}

static bool insert_mapping(struct vma_list *list, const struct vma *vma) {
  if (vma->start >= vma->end) { return false; }
  size_t i = vma_first_ending_after(list, vma->start);
  struct vma *next = i < list->count ? slot_at(list, i) : NULL;
  if (next && next->start < vma->end) { return false; }
  struct vma *prev = i ? slot_at(list, i - 1) : NULL;
  bool join_next = next && vma->end == next->start && compatible(vma, next);
  if (prev && prev->end == vma->start && compatible(prev, vma)) {
    prev->end = join_next ? next->end : vma->end;
    if (join_next) { erase_slots(list, i, i + 1); }
  } else if (join_next) {
    next->start = vma->start;
  } else {
    if (!reserve_slots(list, list->count + 1)) { return false; }
    if (!insert_slot(list, i, vma)) {
      trim_chunks(list);
      return false;
    }
  }
  return true;
}

void vma_list_init(struct vma_list *list) {
  *list = (struct vma_list){0};
}

void vma_list_destroy(struct vma_list *list) {
  if (!list) { return; }
  for (size_t i = 0; i < list->count; ++i) {
    release_mapping(slot_at(list, i));
  }
  list->count = 0;
  trim_chunks(list);
}

const struct vma *vma_lookup(const struct vma_list *list, uint64_t va) {
  if (!list) { return NULL; }
  size_t i = vma_first_ending_after(list, va);
  if (i == list->count) { return NULL; }
  const struct vma *vma = slot_at(list, i);
  return va >= vma->start ? vma : NULL;
}

const struct vma *vma_lookup_range(const struct vma_list *list, uint64_t start, uint64_t end) {
  const struct vma *vma = vma_lookup(list, start);
  return start < end && vma && end <= vma->end ? vma : NULL;
}

bool vma_find_gap(const struct vma_list *list, uint64_t start, uint64_t limit, uint64_t len, uint64_t *out) {
  if (!list || len == 0 || len > limit) { return false; }
  uint64_t page_mask = (uint64_t)PAGE_SIZE - 1;
  uint64_t last_start = (limit - len) & ~page_mask;
  if (start > last_start) { return false; }
  uint64_t base = (start + page_mask) & ~page_mask;
  for (size_t i = vma_first_ending_after(list, base); i < list->count; ++i) {
    const struct vma *vma = slot_at(list, i);
    if (base + len <= vma->start) { break; }
    if (vma->end > last_start) { return false; }
    base = (vma->end + page_mask) & ~page_mask;
  }
  *out = base;
  return true;
}

bool vma_overlaps(const struct vma_list *list, uint64_t start, uint64_t end) {
  if (start >= end) { return true; }
  if (!list) { return false; }
  size_t i = vma_first_ending_after(list, start);
  return i < list->count && slot_at(list, i)->start < end;
}

bool vma_insert(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags,
                enum vma_type type) {
  struct vma vma = {.start = start, .end = end, .prot = prot, .flags = flags, .type = type};
  return insert_mapping(list, &vma);
}

bool vma_insert_file(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags,
                     const struct vfs_node *node, uint64_t file_start, uint64_t file_offset, uint64_t file_size, bool shared_may_write) {
  if (!node || node->is_dir || node->device != RAMFS_DEV_NONE) { return false; }
  struct vma vma = {.start = start,
                    .end = end,
                    .prot = prot,
                    .flags = flags,
                    .type = VMA_FILE,
                    .shared_may_write = shared_may_write,
                    .file_node = *node,
                    .file_start = file_start,
                    .file_offset = file_offset,
                    .file_size = file_size};
  return insert_mapping(list, &vma);
}

bool vma_remove(struct vma_list *list, uint64_t start, uint64_t end) {
  if (start >= end) { return false; }
  size_t i = vma_first_ending_after(list, start);
  if (i == list->count || slot_at(list, i)->start >= end) { return true; }
  struct vma *first = slot_at(list, i);
  if (first->start < start) {
    if (first->end > end) {
      if (!reserve_slots(list, list->count + 1)) { return false; }
      struct vma tail = *first;
      tail.start = end;
      if (!insert_slot(list, i + 1, &tail)) { return false; }
    }
    first->end = start;
    ++i;
  }
  size_t j = i;
  while (j < list->count && slot_at(list, j)->end <= end) {
    ++j;
  }
  if (j < list->count && slot_at(list, j)->start < end) { slot_at(list, j)->start = end; }
  erase_slots(list, i, j);
  return true;
}

static void merge_mappings(struct vma_list *list, size_t first, size_t end) {
  if (first != 0) { --first; }
  if (end < list->count) { ++end; }
  size_t old_count = list->count;
  size_t out = first;
  for (size_t in = first; in < end; ++in) {
    struct vma *current = slot_at(list, in);
    if (out != first) {
      struct vma *previous = slot_at(list, out - 1);
      if (previous->end == current->start && compatible(previous, current)) {
        previous->end = current->end;
        release_mapping(current);
        continue;
      }
    }
    if (out != in) { *slot_at(list, out) = *current; }
    ++out;
  }
  size_t removed = end - out;
  if (removed == 0) { return; }
  for (size_t in = end; in < old_count; ++in) {
    *slot_at(list, out++) = *slot_at(list, in);
  }
  list->count = old_count - removed;
  trim_chunks(list);
}

bool vma_protect(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot) {
  if (start >= end) { return false; }
  size_t first = vma_first_ending_after(list, start);
  size_t last = first;
  size_t additional = 0;
  bool changed = false;
  while (last < list->count) {
    const struct vma *vma = slot_at(list, last);
    if (vma->start >= end) { break; }
    if (vma->prot == prot) {
      ++last;
      continue;
    }
    changed = true;
    uint64_t mid_start = start > vma->start ? start : vma->start;
    uint64_t mid_end = end < vma->end ? end : vma->end;
    additional += (vma->start < mid_start) + (mid_end < vma->end);
    ++last;
  }
  if (!changed) { return true; }
  if (!reserve_slots(list, list->count + additional)) { return false; }

  size_t i = first;
  while (i < list->count) {
    struct vma old = *slot_at(list, i);
    if (old.start >= end) { break; }
    if (old.end <= start || old.prot == prot) {
      ++i;
      continue;
    }
    uint64_t mid_start = start > old.start ? start : old.start;
    uint64_t mid_end = end < old.end ? end : old.end;
    size_t middle = i;
    if (old.start < mid_start) {
      slot_at(list, i)->end = mid_start;
      struct vma part = old;
      part.start = mid_start;
      part.end = mid_end;
      part.prot = prot;
      middle = i + 1;
      insert_slot(list, middle, &part);
    } else {
      slot_at(list, i)->end = mid_end;
      slot_at(list, i)->prot = prot;
    }
    if (mid_end < old.end) {
      struct vma part = old;
      part.start = mid_end;
      insert_slot(list, middle + 1, &part);
    }
    i = middle + 1 + (mid_end < old.end);
  }
  merge_mappings(list, first, i);
  return true;
}

bool vma_extend(struct vma_list *list, uint64_t old_end, uint64_t new_end, bool extend_file) {
  if (!list || !old_end || old_end >= new_end) { return false; }
  size_t i = vma_first_ending_after(list, old_end - 1);
  if (i == list->count || (i + 1 < list->count && slot_at(list, i + 1)->start < new_end)) { return false; }
  struct vma *vma = slot_at(list, i);
  if (vma->end != old_end) { return false; }
  if (extend_file && vma->type == VMA_FILE) {
    uint64_t size = new_end - vma->file_start;
    if (vma->file_offset > UINT64_MAX - size) { return false; }
    vma->file_size = size;
  }
  vma->end = new_end;
  return true;
}

bool vma_grow_down(struct vma_list *list, uint64_t page, uint32_t required_flags, struct vma *out) {
  if (!list || (page & (PAGE_SIZE - 1))) { return false; }
  size_t i = vma_first_ending_after(list, page);
  if (i == list->count) { return false; }
  struct vma *vma = slot_at(list, i);
  if (page >= vma->start || (vma->flags & required_flags) != required_flags || (vma->start - page) / PAGE_SIZE > 2048) {
    return false;
  }
  vma->start = page;
  if (out) { *out = *vma; }
  return true;
}

bool vma_clone(struct vma_list *dst, const struct vma_list *src) {
  vma_list_init(dst);
  if (!reserve_slots(dst, src->count)) { return false; }
  for (size_t i = 0; i < src->count; ++i) {
    if (!insert_slot(dst, i, slot_at(src, i))) {
      vma_list_destroy(dst);
      return false;
    }
  }
  return true;
}

size_t vma_count(const struct vma_list *list) {
  return list ? list->count : 0;
}

uint64_t vma_virtual_pages(const struct vma_list *list) {
  uint64_t pages = 0;
  for (size_t i = 0; i < vma_count(list); ++i) {
    const struct vma *vma = slot_at(list, i);
    pages += (vma->end - vma->start + PAGE_SIZE - 1) / PAGE_SIZE;
  }
  return pages;
}
