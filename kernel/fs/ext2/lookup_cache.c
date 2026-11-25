#include "internal.h"
#include "mm/table.h"
#include "records.h"

enum {
  CHILD_ENTRIES = 8192,
  NAME_BYTES = 256 * 1024,
  INODE_ENTRIES = 4096,
  CACHE_PAGE = 4096,
  PARENT_SETS = 32,
  PARENT_WAYS = 4,
  INITIAL_BUCKETS = 128,
};

struct child_entry {
  uint32_t parent, ino, hash, name_start, block;
  uint16_t next;
  uint8_t len;
};

struct cached_parent {
  uint32_t ino;
  bool complete;
  uint64_t age;
};

enum {
  CHILD_PER_PAGE = CACHE_PAGE / sizeof(struct child_entry),
  INODES_PER_PAGE = CACHE_PAGE / sizeof(struct ext2_node)
};

struct ext2_dir_cache {
  struct ext2_dir_cache *next;
  struct ext2_fs *fs;
  struct child_entry *children[(CHILD_ENTRIES + CHILD_PER_PAGE - 1) / CHILD_PER_PAGE];
  struct ext2_node *inodes[(INODE_ENTRIES + INODES_PER_PAGE - 1) / INODES_PER_PAGE];
  char *names[NAME_BYTES / CACHE_PAGE];
  uint16_t *buckets;
  unsigned bucket_count;
  uint32_t oldest, next_slot, name_end;
  struct cached_parent parents[PARENT_SETS][PARENT_WAYS];
  uint64_t age;
};

static struct ext2_dir_cache *mounted;

static struct ext2_dir_cache *get_cache(struct ext2_fs *fs) {
  if (fs->dir_cache) { return fs->dir_cache; }
  struct ext2_dir_cache *cache = table_alloc(1, sizeof(*cache));
  if (!cache) { return NULL; }
  cache->buckets = table_alloc(INITIAL_BUCKETS, sizeof(*cache->buckets));
  if (!cache->buckets) {
    table_free(cache, 1, sizeof(*cache));
    return NULL;
  }
  cache->bucket_count = INITIAL_BUCKETS;
  cache->fs = fs;
  cache->next = mounted;
  mounted = cache;
  return fs->dir_cache = cache;
}

void ext2_dir_drop_cache(struct ext2_fs *fs) {
  struct ext2_dir_cache **link = &mounted;
  while (*link && (*link)->fs != fs) {
    link = &(*link)->next;
  }
  struct ext2_dir_cache *cache = *link;
  if (!cache) { return; }
  *link = cache->next;
  fs->dir_cache = NULL;
  for (size_t i = 0; i < sizeof(cache->children) / sizeof(*cache->children); ++i) {
    table_free(cache->children[i], CHILD_PER_PAGE, sizeof(struct child_entry));
  }
  for (size_t i = 0; i < sizeof(cache->inodes) / sizeof(*cache->inodes); ++i) {
    table_free(cache->inodes[i], INODES_PER_PAGE, sizeof(struct ext2_node));
  }
  for (size_t i = 0; i < NAME_BYTES / CACHE_PAGE; ++i) {
    table_free(cache->names[i], 1, CACHE_PAGE);
  }
  table_free(cache->buckets, cache->bucket_count, sizeof(*cache->buckets));
  table_free(cache, 1, sizeof(*cache));
}

static uint32_t name_hash(uint32_t parent, const char *name, size_t len) {
  uint32_t hash = parent;
  for (size_t i = 0; i < len; ++i) {
    hash = (hash ^ (uint8_t)name[i]) * 16777619u;
  }
  return hash;
}

static struct child_entry *child_at(struct ext2_dir_cache *cache, unsigned slot) {
  return &cache->children[slot / CHILD_PER_PAGE][slot % CHILD_PER_PAGE];
}

static char *name_at(struct ext2_dir_cache *cache, uint32_t start) {
  return cache->names[(start % NAME_BYTES) / CACHE_PAGE] + start % CACHE_PAGE;
}

static struct child_entry *find_child(struct ext2_dir_cache *cache, uint32_t parent, const char *name, size_t len,
                                      uint32_t hash) {
  if (!cache) { return NULL; }
  for (uint16_t slot = cache->buckets[hash & (cache->bucket_count - 1)]; slot;) {
    struct child_entry *entry = child_at(cache, slot - 1);
    if (entry->hash == hash && entry->parent == parent && entry->len == len &&
        kmemcmp(name_at(cache, entry->name_start), name, len) == 0) {
      return entry;
    }
    slot = entry->next;
  }
  return NULL;
}

static struct cached_parent *parent_for(struct ext2_dir_cache *cache, uint32_t ino, bool create) {
  struct cached_parent *set = cache->parents[(ino ^ (ino >> 8)) % PARENT_SETS], *oldest = set;
  for (size_t i = 0; i < PARENT_WAYS; ++i) {
    if (set[i].ino == ino) { return &set[i]; }
    if (set[i].age < oldest->age) { oldest = &set[i]; }
  }
  if (!create) { return NULL; }
  *oldest = (struct cached_parent){.ino = ino, .complete = true};
  return oldest;
}

static void erase(struct ext2_dir_cache *cache, struct child_entry *entry) {
  uint16_t *slot = &cache->buckets[entry->hash & (cache->bucket_count - 1)];
  while (*slot && child_at(cache, *slot - 1) != entry) {
    slot = &child_at(cache, *slot - 1)->next;
  }
  if (*slot) { *slot = entry->next; }
  entry->ino = 0;
}

static void evict(struct ext2_dir_cache *cache) {
  struct child_entry *entry = child_at(cache, cache->oldest++ % CHILD_ENTRIES);
  if (!entry->ino) { return; }
  struct cached_parent *parent = parent_for(cache, entry->parent, false);
  if (parent) { parent->complete = false; }
  erase(cache, entry);
}

static void grow_buckets(struct ext2_dir_cache *cache) {
  unsigned count = cache->bucket_count * 2;
  uint16_t *buckets = table_alloc(count, sizeof(*buckets));
  if (!buckets) { return; }
  for (uint32_t i = cache->oldest; i != cache->next_slot; ++i) {
    unsigned slot = i % CHILD_ENTRIES;
    struct child_entry *entry = child_at(cache, slot);
    if (!entry->ino) { continue; }
    unsigned bucket = entry->hash & (count - 1);
    entry->next = buckets[bucket];
    buckets[bucket] = (uint16_t)(slot + 1);
  }
  table_free(cache->buckets, cache->bucket_count, sizeof(*buckets));
  cache->buckets = buckets;
  cache->bucket_count = count;
}

void ext2_cache_add_name(struct ext2_fs *fs, uint32_t parent, const char *name, size_t len, uint32_t ino,
                         uint32_t block) {
  struct ext2_dir_cache *cache = get_cache(fs);
  if (!cache) { return; }
  uint32_t hash = name_hash(parent, name, len);
  struct child_entry *entry = find_child(cache, parent, name, len, hash);
  if (!entry) {
    unsigned slot = cache->next_slot % CHILD_ENTRIES;
    uint32_t start = cache->name_end, room = CACHE_PAGE - start % CACHE_PAGE;
    if (len > room) { start += room; }
    char **name_page = &cache->names[(start % NAME_BYTES) / CACHE_PAGE];
    struct child_entry **child_page = &cache->children[slot / CHILD_PER_PAGE];
    if (!*name_page) { *name_page = table_alloc(1, CACHE_PAGE); }
    if (!*child_page) { *child_page = table_alloc(CHILD_PER_PAGE, sizeof(**child_page)); }
    if (!*name_page || !*child_page) {
      struct cached_parent *p = parent_for(cache, parent, false);
      if (p) { p->complete = false; }
      return;
    }
    cache->name_end = start + (uint32_t)len;
    while (cache->oldest != cache->next_slot &&
           (cache->next_slot - cache->oldest >= CHILD_ENTRIES ||
            cache->name_end - child_at(cache, cache->oldest % CHILD_ENTRIES)->name_start > NAME_BYTES)) {
      evict(cache);
    }
    if (cache->bucket_count < CHILD_ENTRIES && cache->next_slot - cache->oldest >= cache->bucket_count) {
      grow_buckets(cache);
    }
    ++cache->next_slot;
    entry = child_at(cache, slot);
    uint32_t bucket = hash & (cache->bucket_count - 1);
    *entry = (struct child_entry){
      .parent = parent, .hash = hash, .len = (uint8_t)len, .name_start = start, .next = cache->buckets[bucket]};
    cache->buckets[bucket] = (uint16_t)(slot + 1);
    kmemcpy(name_at(cache, start), name, len);
  }
  entry->ino = ino;
  entry->block = block;
}

uint32_t ext2_cache_name_block(struct ext2_fs *fs, uint32_t parent, const char *name, size_t len) {
  struct child_entry *entry = find_child(fs->dir_cache, parent, name, len, name_hash(parent, name, len));
  return entry ? entry->block : 0;
}

void ext2_dir_cache_remove_name(struct ext2_fs *fs, uint32_t parent, const char *name) {
  size_t len = kstrlen(name);
  struct child_entry *entry = find_child(fs->dir_cache, parent, name, len, name_hash(parent, name, len));
  if (entry) { erase(fs->dir_cache, entry); }
}

static struct ext2_node *inode_slot(struct ext2_dir_cache *cache, uint32_t ino, bool create) {
  if (!cache) { return NULL; }
  unsigned i = ino % INODE_ENTRIES;
  struct ext2_node **page = &cache->inodes[i / INODES_PER_PAGE];
  if (!*page && create) { *page = table_alloc(INODES_PER_PAGE, sizeof(**page)); }
  return *page ? &(*page)[i % INODES_PER_PAGE] : NULL;
}

bool ext2_read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_node *out) {
  if (!ino) { return false; }
  struct ext2_node *node = inode_slot(get_cache(fs), ino, true);
  if (!node) { return ext2_read_raw_inode(fs, ino, out); }
  if (node->ino != ino && !ext2_read_raw_inode(fs, ino, node)) {
    node->ino = 0;
    return false;
  }
  *out = *node;
  return true;
}

void ext2_cache_written_inode(struct ext2_fs *fs, const struct ext2_node *node) {
  struct ext2_node *cached = inode_slot(fs->dir_cache, node->ino, false);
  if (cached && (!cached->ino || cached->ino == node->ino)) { *cached = *node; }
}

void ext2_dir_cache_invalidate_inode(struct ext2_fs *fs, uint32_t ino) {
  struct ext2_node *node = inode_slot(fs->dir_cache, ino, false);
  if (node && node->ino == ino) { node->ino = 0; }
}

struct child_scan {
  struct ext2_fs *fs;
  uint32_t parent;
  const char *name;
  size_t len;
  uint32_t match;
  bool fill;
  uint64_t cursor;
};

static bool cache_dirent(void *ctx, uint32_t ino, uint8_t type, const char *name, uint8_t len) {
  (void)type;
  struct child_scan *scan = ctx;
  ++ext2_stat_counters.dir_iter_count;
  bool match = scan->len == len && kmemcmp(scan->name, name, len) == 0;
  if (scan->fill || match) {
    ext2_cache_add_name(scan->fs, scan->parent, name, len, ino, scan->cursor / scan->fs->block_size);
  }
  if (match) { scan->match = ino; }
  return match && !scan->fill;
}

int ext2_find_child(struct ext2_fs *fs, const struct ext2_node *dir, const char *name, size_t len,
                    struct ext2_node *out) {
  ++ext2_stat_counters.lookup_child_count;
  struct ext2_dir_cache *cache = get_cache(fs);
  struct child_entry *entry = find_child(cache, dir->ino, name, len, name_hash(dir->ino, name, len));
  if (entry) { return ext2_read_inode(fs, entry->ino, out) ? 1 : -EXT2_EIO; }
  struct child_scan scan = {.fs = fs, .parent = dir->ino, .name = name, .len = len};
  struct cached_parent *parent = NULL;
  if (cache) {
    parent = parent_for(cache, dir->ino, false);
    if (!parent) {
      parent = parent_for(cache, dir->ino, true);
      scan.fill = true;
    }
    parent->age = ++cache->age;
    if (parent->complete && !scan.fill) { return 0; }
  }
  ++ext2_stat_counters.dir_iter_count;
  int rc = ext2_visit_dir(fs, dir, &scan.cursor, cache_dirent, &scan);
  if (rc < 0) {
    if (parent) { parent->complete = false; }
    if (!scan.match) { return -EXT2_EIO; }
  }
  return scan.match ? (ext2_read_inode(fs, scan.match, out) ? 1 : -EXT2_EIO) : 0;
}
