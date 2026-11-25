#include "internal.h"

#include "lib/mem.h"

#include <stddef.h>

enum {
  LOOKUP_CACHE_SETS = 32,
  LOOKUP_CACHE_WAYS = 4,
  LOOKUP_CACHE_PATH_MAX = 255,
  METADATA_BUCKETS = 256,
};

struct vfs_lookup_cache_entry {
  uint32_t generation, metadata_version;
  bool lstat;
  uint64_t age;
  char path[LOOKUP_CACHE_PATH_MAX + 1];
  struct vfs_node node;
};

static struct vfs_lookup_cache_entry entries[LOOKUP_CACHE_SETS][LOOKUP_CACHE_WAYS];
static uint64_t clock;
static uint32_t generation = 1;
static bool populated;
static uint32_t metadata_versions[METADATA_BUCKETS];

static uint32_t *version_for(const struct vfs_node *node) {
  return &metadata_versions[(node->ino ^ node->dev_id) % METADATA_BUCKETS];
}

void vfs_lookup_cache_changed(const struct vfs_node *node) {
  if (populated && !++*version_for(node)) { vfs_lookup_cache_clear(); }
}

static bool same_path(const char *a, const char *b) {
  while (*a && *b) {
    if (*a++ != *b++) { return false; }
  }
  return *a == *b;
}

static bool cache_path(const char *path, bool lstat, size_t *len_out, struct vfs_lookup_cache_entry **set_out) {
  uint32_t hash = lstat ? 2166136261u ^ 1u : 2166136261u;
  size_t len = 0;
  while (path[len] != '\0') {
    if (len == LOOKUP_CACHE_PATH_MAX) { return false; }
    hash = (hash ^ (uint8_t)path[len++]) * 16777619u;
  }
  if (len_out != NULL) { *len_out = len; }
  *set_out = entries[hash & (LOOKUP_CACHE_SETS - 1)];
  return true;
}

static struct vfs_lookup_cache_entry *find_entry(struct vfs_lookup_cache_entry *set, const char *path, bool lstat) {
  for (size_t i = 0; i < LOOKUP_CACHE_WAYS; ++i) {
    if (set[i].generation == generation && set[i].lstat == lstat && same_path(set[i].path, path)) { return &set[i]; }
  }
  return NULL;
}

bool vfs_lookup_cache_get(const char *path, bool lstat, struct vfs_node *out) {
  struct vfs_lookup_cache_entry *set;
  if (!cache_path(path, lstat, NULL, &set)) {
    ++vfs_stat_counters.lookup_cache_misses;
    return false;
  }
  struct vfs_lookup_cache_entry *entry = find_entry(set, path, lstat);
  if (entry != NULL) {
    uint32_t version = *version_for(&entry->node);
    if (entry->metadata_version != version) {
      if (!vfs_refresh(&entry->node, &entry->node)) {
        entry->generation = 0;
        ++vfs_stat_counters.lookup_cache_misses;
        return false;
      }
      entry->metadata_version = version;
    }
    entry->age = ++clock;
    *out = entry->node;
    ++vfs_stat_counters.lookup_cache_hits;
    return true;
  }
  ++vfs_stat_counters.lookup_cache_misses;
  return false;
}

static struct vfs_lookup_cache_entry *victim(struct vfs_lookup_cache_entry *set) {
  struct vfs_lookup_cache_entry *oldest = &set[0];
  for (size_t i = 0; i < LOOKUP_CACHE_WAYS; ++i) {
    if (set[i].generation != generation) { return &set[i]; }
    if (set[i].age < oldest->age) { oldest = &set[i]; }
  }
  return oldest;
}

void vfs_lookup_cache_put(const char *path, bool lstat, const struct vfs_node *node) {
  if (node == NULL || node->backend == VFS_PROC || node->backend == VFS_NSFS) { return; }
  size_t len;
  struct vfs_lookup_cache_entry *set;
  if (!cache_path(path, lstat, &len, &set)) { return; }
  struct vfs_lookup_cache_entry *entry = find_entry(set, path, lstat);
  if (entry == NULL) { entry = victim(set); }
  entry->lstat = lstat;
  kmemcpy(entry->path, path, len + 1);
  entry->node = *node;
  entry->age = ++clock;
  entry->generation = generation;
  entry->metadata_version = *version_for(node);
  populated = true;
}

void vfs_lookup_cache_clear(void) {
  if (!populated) { return; }
  populated = false;
  ++vfs_stat_counters.lookup_cache_invalidations;
  if (++generation == 0) {
    kmemset(entries, 0, sizeof(entries));
    generation = 1;
  }
}
