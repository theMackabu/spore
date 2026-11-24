#include "internal.h"
#include "mm/table.h"

#if __STDC_HOSTED__
#include <stdlib.h>
#else
#include "mm/pool.h"
static struct object_pool inodes;
#endif

static struct ramfs_table *mounts;
constexpr int INITIAL_SLOTS = 128;

static struct ramfs_table **mount_slot(struct ramfs *fs) {
  struct ramfs_table **slot = &mounts;
  while (*slot && (*slot)->owner != fs) { slot = &(*slot)->next; }
  return slot;
}

static size_t table_bytes(int capacity) {
  return sizeof(struct ramfs_table) + (size_t)capacity * sizeof(struct ramfs_slot);
}

static bool resize_table(struct ramfs *fs, int capacity, int high) {
  struct ramfs_table *old = fs->table;
  struct ramfs_table *t = table_alloc(1, table_bytes(capacity));
  if (!t) { return false; }
  t->owner = fs;
  t->reclaim = old ? old->reclaim : NULL;
  t->capacity = capacity;
  t->next_unused = high;
  for (int i = high; i-- > 0;) {
    struct ramfs_mem_node *node = old->slots[i].node;
    t->slots[i].node = node;
    if (!node) {
      t->slots[i].free_next = t->first_free;
      t->first_free = i + 1;
    } else {
      ++t->live;
      if (i && node->parent >= 0) {
        int *bucket = &t->slots[node->name_hash & (capacity - 1)].bucket;
        node->hash_next = *bucket;
        *bucket = i + 1;
      }
    }
  }
  struct ramfs_table **slot = mount_slot(fs);
  t->next = old ? old->next : NULL;
  *slot = fs->table = t;
  if (old) { table_free(old, 1, table_bytes(old->capacity)); }
  return true;
}

static struct ramfs_mem_node *alloc_inode(void) {
#if __STDC_HOSTED__
  return malloc(sizeof(struct ramfs_mem_node));
#else
  if (!inodes.limit && !pool_init(&inodes, sizeof(struct ramfs_mem_node), SIZE_MAX)) { return NULL; }
  return pool_alloc(&inodes);
#endif
}

void ramfs_set_reclaim_callback(struct ramfs *fs, ramfs_reclaim_fn callback) {
  fs->table->reclaim = callback;
}

static void destroy_inode(const struct ramfs_table *t, struct ramfs_mem_node *node) {
  if (t->reclaim && node->link_target < 0) {
    t->reclaim(node->mount, node->ino);
  }
  ramfs_free_node_pages(node);
#if __STDC_HOSTED__
  free(node);
#else
  pool_free(&inodes, node);
#endif
}

int ramfs_alloc_inode(struct ramfs *fs) {
  struct ramfs_table *t = fs->table;
  if (!t || (!t->first_free && t->next_unused == t->capacity)) {
    if (t && t->capacity > INT32_MAX / 2) { return -1; }
    if (!resize_table(fs, t ? t->capacity * 2 : INITIAL_SLOTS, t ? t->next_unused : 0)) { return -1; }
    t = fs->table;
  }
  struct ramfs_mem_node *node = alloc_inode();
  if (!node) { return -1; }
  int i;
  if (t->first_free) {
    i = t->first_free - 1;
    t->first_free = t->slots[i].free_next;
  } else {
    i = t->next_unused++;
  }
  *node = (struct ramfs_mem_node){.parent = -1, .link_target = -1, .links_count = 1, .first_page = -1,
                                  .ino = fs->next_ino++, .atime = ramfs_now_sec, .ctime = ramfs_now_sec,
                                  .mtime = ramfs_now_sec};
  t->slots[i].node = node;
  ++t->live;
  return i;
}

void ramfs_free_inode(struct ramfs *fs, int i) {
  struct ramfs_table *t = fs->table;
  destroy_inode(t, t->slots[i].node);
  t->slots[i].node = NULL;
  t->slots[i].free_next = t->first_free;
  t->first_free = i + 1;
  --t->live;
  if (t->capacity > INITIAL_SLOTS && t->live <= t->capacity / 4 &&
      (t->live == t->capacity / 4 || i == t->next_unused - 1)) {
    int high = t->next_unused;
    while (high && !t->slots[high - 1].node) { --high; }
    int capacity = t->capacity;
    while (capacity > INITIAL_SLOTS && high <= capacity / 2) { capacity /= 2; }
    if (capacity < t->capacity) { (void)resize_table(fs, capacity, high); }
  }
}

void ramfs_destroy(struct ramfs *fs) {
  struct ramfs_table **slot = mount_slot(fs);
  struct ramfs_table *t = *slot;
  if (t) {
    *slot = t->next;
    for (int i = 0; i < t->next_unused; ++i) {
      if (t->slots[i].node) { destroy_inode(t, t->slots[i].node); }
    }
    table_free(t, 1, table_bytes(t->capacity));
  }
  *fs = (struct ramfs){0};
}
