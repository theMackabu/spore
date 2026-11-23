#include "internal.h"

#include "lib/mem.h"

#include <stddef.h>

enum {
  LOOKUP_CACHE_SETS = 32,
  LOOKUP_CACHE_WAYS = 4,
  LOOKUP_CACHE_ENTRIES = LOOKUP_CACHE_SETS * LOOKUP_CACHE_WAYS,
  LOOKUP_CACHE_PATH_MAX = 255,
};

struct vfs_lookup_cache_entry {
  bool valid;
  bool lstat;
  uint64_t age;
  char path[LOOKUP_CACHE_PATH_MAX + 1];
  struct vfs_node node;
};

static struct vfs_lookup_cache_entry entries[LOOKUP_CACHE_SETS][LOOKUP_CACHE_WAYS];
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

static uint32_t cache_set(const char *path, bool lstat) {
  uint32_t hash = lstat ? 2166136261u ^ 1u : 2166136261u;
  while (*path != '\0') {
    hash = (hash ^ (uint8_t)*path++) * 16777619u;
  }
  return hash & (LOOKUP_CACHE_SETS - 1);
}

static struct vfs_lookup_cache_entry *find_entry(const char *path, bool lstat) {
  struct vfs_lookup_cache_entry *set = entries[cache_set(path, lstat)];
  for (size_t i = 0; i < LOOKUP_CACHE_WAYS; ++i) {
    if (set[i].valid && set[i].lstat == lstat && same_path(set[i].path, path)) { return &set[i]; }
  }
  return NULL;
}

bool vfs_lookup_cache_get(const char *path, bool lstat, struct vfs_node *out) {
  struct vfs_lookup_cache_entry *entry = find_entry(path, lstat);
  if (entry != NULL) {
    entry->age = ++clock;
    *out = entry->node;
    ++vfs_stat_counters.lookup_cache_hits;
    return true;
  }
  ++vfs_stat_counters.lookup_cache_misses;
  return false;
}

static struct vfs_lookup_cache_entry *victim(const char *path, bool lstat) {
  struct vfs_lookup_cache_entry *set = entries[cache_set(path, lstat)];
  struct vfs_lookup_cache_entry *oldest = &set[0];
  for (size_t i = 0; i < LOOKUP_CACHE_WAYS; ++i) {
    if (!set[i].valid) { return &set[i]; }
    if (set[i].age < oldest->age) { oldest = &set[i]; }
  }
  return oldest;
}

void vfs_lookup_cache_put(const char *path, bool lstat, const struct vfs_node *node) {
  if (node == NULL || node->backend == VFS_PROC) { return; }
  struct vfs_lookup_cache_entry *entry = find_entry(path, lstat);
  if (entry == NULL) { entry = victim(path, lstat); }
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
  for (size_t set = 0; set < LOOKUP_CACHE_SETS; ++set) {
    for (size_t way = 0; way < LOOKUP_CACHE_WAYS; ++way) {
      if (entries[set][way].valid) {
        entries[set][way].valid = false;
        any = true;
      }
    }
  }
  if (any) { ++vfs_stat_counters.lookup_cache_invalidations; }
}
