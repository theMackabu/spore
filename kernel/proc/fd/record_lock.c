#include "record_lock.h"
#include "mm/pool.h"
#include "proc/fd.h"

struct record {
  struct record_lock_range range;
  uint64_t ino, dev;
  enum vfs_backend backend;
  struct record *next;
};

static struct record *records;
static struct object_pool pool;

static bool same_inode(const struct record *lock, const struct open_file *file) {
  return lock->ino == file->node.ino && lock->dev == file->node.dev_id && lock->backend == file->node.backend;
}

static bool overlaps(const struct record_lock_range *a, const struct record_lock_range *b) {
  return a->start < b->end && b->start < a->end;
}

static void remove_record(struct record **slot) {
  struct record *old = *slot;
  *slot = old->next;
  pool_free(&pool, old);
}

void cell_record_unlock_file(int owner, const struct open_file *file) {
  if (!records || file->type != OPEN_VFS) { return; }
  struct record **slot = &records;
  while (*slot) {
    if ((*slot)->range.owner == owner && same_inode(*slot, file)) {
      remove_record(slot);
    } else {
      slot = &(*slot)->next;
    }
  }
}

int cell_record_lock(struct open_file *file, struct record_lock_range *range, bool query) {
  struct record *split = NULL;
  for (struct record *p = records; p; p = p->next) {
    if (!same_inode(p, file) || !overlaps(&p->range, range)) { continue; }
    if (p->range.owner != range->owner) {
      if (range->type == RECORD_UNLOCK || (range->type == RECORD_READ && p->range.type == RECORD_READ)) { continue; }
      if (!query) { return -11; }
      *range = p->range;
      return 0;
    }
    if (!query && p->range.start <= range->start && p->range.end >= range->end && p->range.type == range->type) {
      return 0;
    }
    if (p->range.start < range->start && range->end < p->range.end && p->range.type != range->type) { split = p; }
  }
  if (query) {
    range->type = RECORD_UNLOCK;
    return 0;
  }
  if (!pool.limit) { pool_init(&pool, sizeof(struct record), SIZE_MAX); }
  struct record *added = NULL;
  if (range->type != RECORD_UNLOCK) {
    added = pool_alloc(&pool);
    if (!added) { return -37; }
    *added =
      (struct record){.range = *range, .ino = file->node.ino, .dev = file->node.dev_id, .backend = file->node.backend};
  }
  if (split) {
    struct record *right = pool_alloc(&pool);
    if (!right) {
      if (added) { pool_free(&pool, added); }
      return -37;
    }
    *right = *split;
    right->range.start = range->end;
    split->range.end = range->start;
    split->next = right;
  }
  struct record **slot = &records;
  while (*slot) {
    struct record *p = *slot;
    if (p->range.owner == range->owner && same_inode(p, file)) {
      if (added && p->range.type == range->type && p->range.start <= range->end && range->start <= p->range.end) {
        if (p->range.start < added->range.start) { added->range.start = p->range.start; }
        if (p->range.end > added->range.end) { added->range.end = p->range.end; }
        remove_record(slot);
        continue;
      }
      if (overlaps(&p->range, range)) {
        if (p->range.start < range->start) {
          p->range.end = range->start;
        } else if (p->range.end > range->end) {
          p->range.start = range->end;
        } else {
          remove_record(slot);
          continue;
        }
      }
    }
    slot = &p->next;
  }
  if (added) {
    added->next = records;
    records = added;
  }
  return 0;
}
