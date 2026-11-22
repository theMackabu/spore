#include "vfs_internal.h"

#include "mem.h"

#include <stddef.h>

enum { LOOKUP_CACHE_ENTRIES = 128, LOOKUP_CACHE_PATH_MAX = 255 };

struct vfs_lookup_cache_entry {
  bool valid;
  bool lstat;
  uint64_t age;
  char path[LOOKUP_CACHE_PATH_MAX + 1];
  struct vfs_node node;
};

static struct vfs_lookup_cache_entry entries[LOOKUP_CACHE_ENTRIES];
static uint64_t clock;

static bool same_path(const char *a, const char *b) {
  while (*a && *b) {
    if (*a++ != *b++) { return false; }
  }
  return *a == *b;
}

static bool copy_cache_path(char *dst, const char *path) {
  size_t len = kstrlen(path);
  if (len > LOOKUP_CACHE_PATH_MAX) { return false; }
  kmemcpy(dst, path, len + 1);
  return true;
}

bool vfs_lookup_cache_get(const char *path, bool lstat, struct vfs_node *out) {
  for (size_t i = 0; i < LOOKUP_CACHE_ENTRIES; ++i) {
    if (entries[i].valid && entries[i].lstat == lstat && same_path(entries[i].path, path)) {
      entries[i].age = ++clock;
      *out = entries[i].node;
      ++vfs_stat_counters.lookup_cache_hits;
      return true;
    }
  }
  ++vfs_stat_counters.lookup_cache_misses;
  return false;
}

static struct vfs_lookup_cache_entry *victim(void) {
  struct vfs_lookup_cache_entry *victim = &entries[0];
  for (size_t i = 0; i < LOOKUP_CACHE_ENTRIES; ++i) {
    if (!entries[i].valid) { return &entries[i]; }
    if (entries[i].age < victim->age) { victim = &entries[i]; }
  }
  return victim;
}

void vfs_lookup_cache_put(const char *path, bool lstat, const struct vfs_node *node) {
  if (node == NULL || node->backend == VFS_PROC) { return; }
  struct vfs_lookup_cache_entry *entry = victim();
  char path_copy[LOOKUP_CACHE_PATH_MAX + 1];
  if (!copy_cache_path(path_copy, path)) { return; }
  entry->valid = false;
  entry->lstat = lstat;
  kmemcpy(entry->path, path_copy, kstrlen(path_copy) + 1);
  entry->node = *node;
  entry->age = ++clock;
  entry->valid = true;
}

void vfs_lookup_cache_clear(void) {
  bool any = false;
  for (size_t i = 0; i < LOOKUP_CACHE_ENTRIES; ++i) {
    if (entries[i].valid) {
      entries[i].valid = false;
      any = true;
    }
  }
  if (any) { ++vfs_stat_counters.lookup_cache_invalidations; }
}
