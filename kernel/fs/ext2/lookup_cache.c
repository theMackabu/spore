#include "internal.h"
#include "records.h"

enum {
  CHILD_CACHE_ENTRIES = 8192,
  CHILD_CACHE_BUCKETS = 8192,
  CHILD_NAME_BYTES = 256 * 1024,
  INODE_CACHE_ENTRIES = 4096,
  CHILD_PARENT_CACHE_ENTRIES = 128,
};

struct child_cache_entry {
  struct ext2_fs *fs;
  uint32_t parent_ino;
  uint32_t ino;
  uint32_t hash;
  uint32_t name_start;
  uint16_t next;
  uint8_t name_len;
};

struct cached_inode {
  struct ext2_fs *fs;
  struct ext2_node node;
};

struct child_parent_cache_entry {
  struct ext2_fs *fs;
  uint32_t parent_ino;
  bool complete;
  uint64_t age;
};

static struct child_cache_entry child_cache[CHILD_CACHE_ENTRIES];
static uint16_t child_buckets[CHILD_CACHE_BUCKETS];
static char child_names[CHILD_NAME_BYTES];
static uint32_t oldest_slot, next_slot, name_end;
static struct cached_inode inode_cache[INODE_CACHE_ENTRIES];
static struct child_parent_cache_entry child_parent_cache[CHILD_PARENT_CACHE_ENTRIES];
static uint64_t child_cache_clock;

static uint32_t child_name_hash(uint32_t parent, const char *name, size_t len) {
  uint32_t hash = parent;
  for (size_t i = 0; i < len; ++i) {
    hash = (hash ^ (uint8_t)name[i]) * 16777619u;
  }
  return hash;
}

static struct child_cache_entry *child_cache_find(struct ext2_fs *fs, uint32_t parent_ino, const char *name, size_t len,
                                                  uint32_t hash) {
  uint16_t slot = child_buckets[hash % CHILD_CACHE_BUCKETS];
  while (slot) {
    struct child_cache_entry *entry = &child_cache[slot - 1];
    if (entry->hash == hash && entry->fs == fs && entry->parent_ino == parent_ino && entry->name_len == len &&
        kmemcmp(child_names + entry->name_start % CHILD_NAME_BYTES, name, len) == 0) {
      return entry;
    }
    slot = entry->next;
  }
  return NULL;
}

static void child_cache_erase(struct child_cache_entry *entry) {
  uint16_t *slot = &child_buckets[entry->hash % CHILD_CACHE_BUCKETS];
  while (*slot && &child_cache[*slot - 1] != entry) {
    slot = &child_cache[*slot - 1].next;
  }
  if (*slot) { *slot = entry->next; }
  entry->fs = NULL;
}

void ext2_dir_drop_cache(struct ext2_fs *fs) {
  for (size_t i = 0; i < CHILD_CACHE_ENTRIES; ++i) {
    if (child_cache[i].fs == fs) { child_cache_erase(&child_cache[i]); }
  }
  for (size_t i = 0; i < INODE_CACHE_ENTRIES; ++i) {
    if (inode_cache[i].fs == fs) { inode_cache[i].fs = NULL; }
  }
  for (size_t i = 0; i < CHILD_PARENT_CACHE_ENTRIES; ++i) {
    if (child_parent_cache[i].fs == fs) { child_parent_cache[i].fs = NULL; }
  }
}

static struct child_parent_cache_entry *child_parent_cache_find(struct ext2_fs *fs, uint32_t parent_ino) {
  for (size_t i = 0; i < CHILD_PARENT_CACHE_ENTRIES; ++i) {
    struct child_parent_cache_entry *entry = &child_parent_cache[i];
    if (entry->fs == fs && entry->parent_ino == parent_ino) { return entry; }
  }
  return NULL;
}

static struct child_parent_cache_entry *child_parent_cache_alloc_slot(void) {
  struct child_parent_cache_entry *entry = &child_parent_cache[0];
  for (size_t i = 0; i < CHILD_PARENT_CACHE_ENTRIES; ++i) {
    if (child_parent_cache[i].fs == NULL) { return &child_parent_cache[i]; }
    if (child_parent_cache[i].age < entry->age) { entry = &child_parent_cache[i]; }
  }
  return entry;
}

static void evict_oldest(void) {
  struct child_cache_entry *entry = &child_cache[oldest_slot++ % CHILD_CACHE_ENTRIES];
  if (entry->fs == NULL) { return; }
  struct child_parent_cache_entry *parent = child_parent_cache_find(entry->fs, entry->parent_ino);
  if (parent != NULL) { parent->complete = false; }
  child_cache_erase(entry);
}

void ext2_cache_add_name(struct ext2_fs *fs, uint32_t parent_ino, const char *name, size_t len, uint32_t ino) {
  uint32_t hash = child_name_hash(parent_ino, name, len);
  struct child_cache_entry *entry = child_cache_find(fs, parent_ino, name, len, hash);
  if (entry == NULL) {
    uint32_t start = name_end;
    uint32_t space = CHILD_NAME_BYTES - start % CHILD_NAME_BYTES;
    if (len > space) { start += space; }
    name_end = start + (uint32_t)len;
    while (oldest_slot != next_slot &&
           (next_slot - oldest_slot >= CHILD_CACHE_ENTRIES ||
            name_end - child_cache[oldest_slot % CHILD_CACHE_ENTRIES].name_start > CHILD_NAME_BYTES)) {
      evict_oldest();
    }
    entry = &child_cache[next_slot++ % CHILD_CACHE_ENTRIES];
    entry->fs = fs;
    entry->parent_ino = parent_ino;
    entry->hash = hash;
    entry->name_start = start;
    entry->name_len = (uint8_t)len;
    kmemcpy(child_names + start % CHILD_NAME_BYTES, name, len);
    uint16_t *head = &child_buckets[hash % CHILD_CACHE_BUCKETS];
    entry->next = *head;
    *head = (uint16_t)(entry - child_cache + 1);
  }
  entry->ino = ino;
}

static bool child_cache_read_entry(struct ext2_fs *fs, uint32_t ino, struct ext2_node *out) {
  struct cached_inode *entry = &inode_cache[ino % INODE_CACHE_ENTRIES];
  if (entry->fs != fs || entry->node.ino != ino) {
    entry->fs = NULL;
    if (!ext2_read_inode(fs, ino, &entry->node)) { return false; }
    entry->fs = fs;
  }
  *out = entry->node;
  return true;
}

void ext2_dir_cache_remove_name(struct ext2_fs *fs, uint32_t parent_ino, const char *name) {
  size_t len = kstrlen(name);
  struct child_cache_entry *entry = child_cache_find(fs, parent_ino, name, len, child_name_hash(parent_ino, name, len));
  if (entry) { child_cache_erase(entry); }
}

void ext2_dir_cache_invalidate_inode(struct ext2_fs *fs, uint32_t ino) {
  struct cached_inode *entry = &inode_cache[ino % INODE_CACHE_ENTRIES];
  if (entry->fs == fs && entry->node.ino == ino) { entry->fs = NULL; }
}

struct child_scan {
  struct ext2_fs *fs;
  uint32_t parent;
  const char *name;
  size_t name_len;
  uint32_t matched_ino;
  bool fill;
};

static bool cache_dirent(void *ctx, uint32_t ino, uint8_t type, const char *name, uint8_t len) {
  (void)type;
  struct child_scan *scan = ctx;
  ++ext2_stat_counters.dir_iter_count;
  bool match = scan->name_len == len && kmemcmp(scan->name, name, len) == 0;
  if (scan->fill || match) { ext2_cache_add_name(scan->fs, scan->parent, name, len, ino); }
  if (match) { scan->matched_ino = ino; }
  return match && !scan->fill;
}

bool ext2_lookup_child(struct ext2_fs *fs, const struct ext2_node *dir, const char *name, size_t name_len,
                       struct ext2_node *out) {
  ++ext2_stat_counters.lookup_child_count;
  struct child_cache_entry *cached =
    child_cache_find(fs, dir->ino, name, name_len, child_name_hash(dir->ino, name, name_len));
  if (cached != NULL) { return child_cache_read_entry(fs, cached->ino, out); }
  struct child_parent_cache_entry *parent = child_parent_cache_find(fs, dir->ino);
  struct child_scan scan = {.fs = fs, .parent = dir->ino, .name = name, .name_len = name_len, .fill = parent == NULL};
  if (parent == NULL) {
    parent = child_parent_cache_alloc_slot();
    *parent = (struct child_parent_cache_entry){.fs = fs, .parent_ino = dir->ino, .complete = true};
  } else if (parent->complete) {
    parent->age = ++child_cache_clock;
    return false;
  }
  parent->age = ++child_cache_clock;
  uint64_t cursor = 0;
  ++ext2_stat_counters.dir_iter_count;
  if (ext2_visit_dir(fs, dir, &cursor, cache_dirent, &scan) < 0) { parent->complete = false; }
  return scan.matched_ino != 0 && child_cache_read_entry(fs, scan.matched_ino, out);
}
