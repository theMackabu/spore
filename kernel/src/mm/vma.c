#include "mm/vma.h"

#include "mem.h"

static bool overlaps(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end) {
  return a_start < b_end && b_start < a_end;
}

static bool compatible(const struct vma *a, const struct vma *b) {
  return a->prot == b->prot && a->flags == b->flags && a->type == b->type;
}

static bool add_raw(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags,
                    enum vma_type type) {
  if (start >= end) { return false; }
  for (size_t i = 0; i < MAX_VMAS; ++i) {
    if (!list->entries[i].used) {
      list->entries[i] = (struct vma){
        .used = true,
        .start = start,
        .end = end,
        .prot = prot,
        .flags = flags,
        .type = type,
      };
      return true;
    }
  }
  return false;
}

static void merge_adjacent(struct vma_list *list) {
  for (size_t i = 0; i < MAX_VMAS; ++i) {
    if (!list->entries[i].used) { continue; }
    for (size_t j = 0; j < MAX_VMAS; ++j) {
      if (i == j || !list->entries[j].used) { continue; }
      if (list->entries[i].end == list->entries[j].start && compatible(&list->entries[i], &list->entries[j])) {
        list->entries[i].end = list->entries[j].end;
        list->entries[j].used = false;
        j = 0;
      } else if (list->entries[j].end == list->entries[i].start && compatible(&list->entries[i], &list->entries[j])) {
        list->entries[i].start = list->entries[j].start;
        list->entries[j].used = false;
        j = 0;
      }
    }
  }
}

void vma_list_init(struct vma_list *list) {
  kmemset(list, 0, sizeof(*list));
}

const struct vma *vma_lookup(const struct vma_list *list, uint64_t va) {
  for (size_t i = 0; i < MAX_VMAS; ++i) {
    const struct vma *vma = &list->entries[i];
    if (vma->used && va >= vma->start && va < vma->end) { return vma; }
  }
  return NULL;
}

bool vma_insert(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags,
                enum vma_type type) {
  if (start >= end) { return false; }
  for (size_t i = 0; i < MAX_VMAS; ++i) {
    const struct vma *vma = &list->entries[i];
    if (vma->used && overlaps(start, end, vma->start, vma->end)) { return false; }
  }
  if (!add_raw(list, start, end, prot, flags, type)) { return false; }
  merge_adjacent(list);
  return true;
}

bool vma_remove(struct vma_list *list, uint64_t start, uint64_t end) {
  if (start >= end) { return false; }

  struct vma_list next;
  vma_list_init(&next);
  for (size_t i = 0; i < MAX_VMAS; ++i) {
    const struct vma *vma = &list->entries[i];
    if (!vma->used) { continue; }
    if (!overlaps(start, end, vma->start, vma->end)) {
      if (!add_raw(&next, vma->start, vma->end, vma->prot, vma->flags, vma->type)) { return false; }
      continue;
    }
    if (vma->start < start && !add_raw(&next, vma->start, start, vma->prot, vma->flags, vma->type)) { return false; }
    if (end < vma->end && !add_raw(&next, end, vma->end, vma->prot, vma->flags, vma->type)) { return false; }
  }
  merge_adjacent(&next);
  *list = next;
  return true;
}

bool vma_protect(struct vma_list *list, uint64_t start, uint64_t end, uint32_t prot) {
  if (start >= end) { return false; }

  struct vma_list next;
  vma_list_init(&next);
  for (size_t i = 0; i < MAX_VMAS; ++i) {
    const struct vma *vma = &list->entries[i];
    if (!vma->used) { continue; }
    if (!overlaps(start, end, vma->start, vma->end)) {
      if (!add_raw(&next, vma->start, vma->end, vma->prot, vma->flags, vma->type)) { return false; }
      continue;
    }
    if (vma->start < start && !add_raw(&next, vma->start, start, vma->prot, vma->flags, vma->type)) { return false; }
    uint64_t mid_start = vma->start > start ? vma->start : start;
    uint64_t mid_end = vma->end < end ? vma->end : end;
    if (!add_raw(&next, mid_start, mid_end, prot, vma->flags, vma->type)) { return false; }
    if (end < vma->end && !add_raw(&next, end, vma->end, vma->prot, vma->flags, vma->type)) { return false; }
  }
  merge_adjacent(&next);
  *list = next;
  return true;
}

bool vma_clone(struct vma_list *dst, const struct vma_list *src) {
  *dst = *src;
  return true;
}

size_t vma_count(const struct vma_list *list) {
  size_t count = 0;
  for (size_t i = 0; i < MAX_VMAS; ++i) {
    if (list->entries[i].used) { ++count; }
  }
  return count;
}
