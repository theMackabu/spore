#include "internal.h"
#include "rust.h"

enum {
  CHILD_CACHE_ENTRIES = 4096,
  CHILD_CACHE_BUCKETS = 4096,
  CHILD_PARENT_CACHE_ENTRIES = 128,
  SYMLINK_DEPTH_MAX = 8,
};

struct child_cache_entry {
  struct ext2_fs *fs;
  uint32_t parent_ino;
  uint32_t ino;
  uint16_t next;
  uint8_t name_len;
  char name[EXT2_NAME_MAX];
};

struct cached_inode {
  struct ext2_fs *fs;
  struct ext2_node node;
};

struct child_parent_cache_entry {
  bool valid;
  bool complete;
  struct ext2_fs *fs;
  uint32_t parent_ino;
  uint64_t age;
};

static struct child_cache_entry child_cache[CHILD_CACHE_ENTRIES];
static struct child_parent_cache_entry child_parent_cache[CHILD_PARENT_CACHE_ENTRIES];
static uint64_t child_cache_clock;
static uint16_t child_buckets[CHILD_CACHE_BUCKETS];
static size_t child_next_slot;
static struct cached_inode inode_cache[CHILD_CACHE_ENTRIES];

static uint16_t rec_len_for(uint8_t name_len) {
  return (uint16_t)((8u + name_len + 3u) & ~3u);
}

static void write_dir_record(uint8_t *p, uint32_t ino, uint16_t rec_len, uint8_t name_len, uint8_t type,
                             const char *name) {
  p[0] = (uint8_t)ino;
  p[1] = (uint8_t)(ino >> 8);
  p[2] = (uint8_t)(ino >> 16);
  p[3] = (uint8_t)(ino >> 24);
  p[4] = (uint8_t)rec_len;
  p[5] = (uint8_t)(rec_len >> 8);
  p[6] = name_len;
  p[7] = type;
  kmemcpy(p + 8, name, name_len);
}

static uint8_t dirent_type_for_node(const struct ext2_node *node) {
  if (ext2_is_dir(node)) { return EXT2_FT_DIR; }
  if (ext2_is_symlink(node)) { return EXT2_FT_SYMLINK; }
  return EXT2_FT_REG_FILE;
}


static bool ext2plus_hidden_name(struct ext2_fs *fs, const struct ext2_node *dir, const char *name, size_t name_len) {
  const char hidden[] = ".spore-inodes";
  return fs->ext2plus && dir->ino == EXT2_ROOT_INO && name_len == sizeof(hidden) - 1 &&
         kmemcmp(name, hidden, sizeof(hidden) - 1) == 0;
}

static int read_next_dirent(struct ext2_fs *fs, const struct ext2_node *dir, uint64_t *cursor, struct ext2_dirent *out) {
  ++ext2_stat_counters.dir_iter_count;
  if (!ext2_is_dir(dir)) { return -EXT2_EIO; }
  while (*cursor < dir->size) {
    uint32_t file_block_index = (uint32_t)(*cursor / fs->block_size);
    uint32_t within = (uint32_t)(*cursor % fs->block_size);
    if (fs->block_size - within < 8 || dir->size - *cursor < 8) { return -EXT2_EIO; }
    uint32_t disk_block = 0;
    if (!ext2_file_block(fs, dir, file_block_index, &disk_block) || disk_block == 0) { return -EXT2_EIO; }
    const uint8_t *block = ext2_cached_block(fs, disk_block);
    if (block == NULL) { return -EXT2_EIO; }
    size_t available = fs->block_size - within;
    if (available > dir->size - *cursor) { available = dir->size - *cursor; }
    struct spore_ext2_dir_next next;
    if (spore_ext2_next_visible_dirent(block + within, available, fs->ext2plus && dir->ino == EXT2_ROOT_INO, &next) < 0 ||
        next.consumed == 0) { return -EXT2_EIO; }
    *cursor += next.consumed;
    if (!next.found) { continue; }
    if (out != NULL) {
      kmemcpy(out->name, block + within + next.name_offset, next.name_len);
      out->name[next.name_len] = '\0';
      out->ino = next.ino;
      out->type = next.type;
    }
    return 1;
  }
  return 0;
}

bool ext2_next_dirent(struct ext2_fs *fs, const struct ext2_node *dir, uint64_t *cursor, struct ext2_dirent *out) {
  uint64_t start = 0;
  return read_next_dirent(fs, dir, cursor != NULL ? cursor : &start, out) == 1;
}

bool ext2_dirent(struct ext2_fs *fs, const struct ext2_node *dir, size_t index, struct ext2_dirent *out) {
  uint64_t cursor = 0;
  for (size_t i = 0; ext2_next_dirent(fs, dir, &cursor, out); ++i) {
    if (i == index) { return true; }
  }
  return false;
}

static bool name_equals(const char *a, size_t a_len, const char *b) {
  if (kstrlen(b) != a_len) { return false; }
  return kmemcmp(a, b, a_len) == 0;
}

static uint32_t child_name_bucket(uint32_t parent, const char *name, size_t len) {
  uint32_t hash = parent;
  for (size_t i = 0; i < len; ++i) {
    hash = (hash ^ (uint8_t)name[i]) * 16777619u;
  }
  return hash % CHILD_CACHE_BUCKETS;
}

static struct child_cache_entry *child_cache_find(struct ext2_fs *fs, uint32_t parent_ino, const char *name,
                                                  size_t len) {
  uint16_t slot = child_buckets[child_name_bucket(parent_ino, name, len)];
  while (slot) {
    struct child_cache_entry *entry = &child_cache[slot - 1];
    if (entry->fs == fs && entry->parent_ino == parent_ino && entry->name_len == len &&
        kmemcmp(entry->name, name, len) == 0) {
      return entry;
    }
    slot = entry->next;
  }
  return NULL;
}

static void child_cache_erase(struct child_cache_entry *entry) {
  uint16_t *slot = &child_buckets[child_name_bucket(entry->parent_ino, entry->name, entry->name_len)];
  while (*slot && &child_cache[*slot - 1] != entry) {
    slot = &child_cache[*slot - 1].next;
  }
  if (*slot) { *slot = entry->next; }
  entry->fs = NULL;
}

void ext2_dir_drop_cache(struct ext2_fs *fs) {
  for (size_t i = 0; i < CHILD_CACHE_ENTRIES; ++i) {
    if (child_cache[i].fs == fs) { child_cache_erase(&child_cache[i]); }
    if (inode_cache[i].fs == fs) { inode_cache[i].fs = NULL; }
  }
  for (size_t i = 0; i < CHILD_PARENT_CACHE_ENTRIES; ++i) {
    if (child_parent_cache[i].fs == fs) { child_parent_cache[i].valid = false; }
  }
}

static struct child_parent_cache_entry *child_parent_cache_find(struct ext2_fs *fs, uint32_t parent_ino) {
  for (size_t i = 0; i < CHILD_PARENT_CACHE_ENTRIES; ++i) {
    struct child_parent_cache_entry *entry = &child_parent_cache[i];
    if (entry->valid && entry->fs == fs && entry->parent_ino == parent_ino) { return entry; }
  }
  return NULL;
}

static struct child_parent_cache_entry *child_parent_cache_alloc_slot(void) {
  struct child_parent_cache_entry *entry = &child_parent_cache[0];
  for (size_t i = 0; i < CHILD_PARENT_CACHE_ENTRIES; ++i) {
    if (!child_parent_cache[i].valid) { return &child_parent_cache[i]; }
    if (child_parent_cache[i].age < entry->age) { entry = &child_parent_cache[i]; }
  }
  return entry;
}

static bool child_parent_cache_complete(struct ext2_fs *fs, uint32_t parent_ino) {
  struct child_parent_cache_entry *entry = child_parent_cache_find(fs, parent_ino);
  if (entry == NULL || !entry->complete) { return false; }
  entry->age = ++child_cache_clock;
  return true;
}

static void child_parent_cache_set(struct ext2_fs *fs, uint32_t parent_ino, bool complete) {
  struct child_parent_cache_entry *entry = child_parent_cache_find(fs, parent_ino);
  if (entry == NULL) { entry = child_parent_cache_alloc_slot(); }
  entry->valid = true;
  entry->complete = complete;
  entry->fs = fs;
  entry->parent_ino = parent_ino;
  entry->age = ++child_cache_clock;
}

static void child_cache_store(struct ext2_fs *fs, uint32_t parent_ino, const char *name, size_t len, uint32_t ino) {
  struct child_cache_entry *entry = child_cache_find(fs, parent_ino, name, len);
  if (entry == NULL) {
    entry = &child_cache[child_next_slot++ % CHILD_CACHE_ENTRIES];
    if (entry->fs != NULL) {
      child_parent_cache_set(entry->fs, entry->parent_ino, false);
      child_cache_erase(entry);
    }
    entry->fs = fs;
    entry->parent_ino = parent_ino;
    entry->name_len = (uint8_t)len;
    kmemcpy(entry->name, name, len);
    uint32_t bucket = child_name_bucket(parent_ino, name, len);
    entry->next = child_buckets[bucket];
    child_buckets[bucket] = (uint16_t)(entry - child_cache + 1);
  }
  entry->ino = ino;
}

static bool child_cache_read_entry(struct ext2_fs *fs, uint32_t ino, struct ext2_node *out) {
  struct cached_inode *entry = &inode_cache[ino % CHILD_CACHE_ENTRIES];
  if (entry->fs != fs || entry->node.ino != ino) {
    entry->fs = NULL;
    if (!ext2_read_inode(fs, ino, &entry->node)) { return false; }
    entry->fs = fs;
  }
  *out = entry->node;
  return true;
}

void ext2_dir_cache_remove_name(struct ext2_fs *fs, uint32_t parent_ino, const char *name) {
  struct child_cache_entry *entry = child_cache_find(fs, parent_ino, name, kstrlen(name));
  if (entry) { child_cache_erase(entry); }
}

void ext2_dir_cache_invalidate_inode(struct ext2_fs *fs, uint32_t ino) {
  struct cached_inode *entry = &inode_cache[ino % CHILD_CACHE_ENTRIES];
  if (entry->fs == fs && entry->node.ino == ino) { entry->fs = NULL; }
}

static bool lookup_child(struct ext2_fs *fs, const struct ext2_node *dir, const char *name, size_t name_len,
                         struct ext2_node *out) {
  ++ext2_stat_counters.lookup_child_count;
  struct child_cache_entry *cached = child_cache_find(fs, dir->ino, name, name_len);
  if (cached != NULL) { return child_cache_read_entry(fs, cached->ino, out); }
  if (child_parent_cache_complete(fs, dir->ino)) { return false; }
  struct ext2_dirent ent;
  uint64_t cursor = 0;
  bool found = false;
  child_parent_cache_set(fs, dir->ino, true);
  while (ext2_next_dirent(fs, dir, &cursor, &ent)) {
    child_cache_store(fs, dir->ino, ent.name, kstrlen(ent.name), ent.ino);
    if (name_equals(name, name_len, ent.name)) { found = child_cache_read_entry(fs, ent.ino, out); }
  }
  if (cursor < dir->size) { child_parent_cache_set(fs, dir->ino, false); }
  return found;
}

static bool split_parent_path(const char *path, char *parent, size_t parent_cap, char *name, size_t name_cap) {
  if (path == NULL || path[0] != '/') { return false; }
  size_t len = kstrlen(path);
  while (len > 1 && path[len - 1] == '/') {
    --len;
  }
  size_t slash = len;
  while (slash > 0 && path[slash - 1] != '/') {
    --slash;
  }
  size_t name_len = len - slash;
  if (name_len == 0 || name_len >= name_cap) { return false; }
  kmemcpy(name, path + slash, name_len);
  name[name_len] = '\0';
  size_t parent_len = slash <= 1 ? 1 : slash - 1;
  if (parent_len >= parent_cap) { return false; }
  kmemcpy(parent, path, parent_len);
  parent[parent_len] = '\0';
  return true;
}

static bool append_path_component(char *path, size_t cap, const char *name, size_t name_len) {
  size_t len = kstrlen(path);
  if (len == 0) { return false; }
  if (!(len == 1 && path[0] == '/')) {
    if (len + 1 >= cap) { return false; }
    path[len++] = '/';
  }
  if (len + name_len >= cap) { return false; }
  kmemcpy(path + len, name, name_len);
  path[len + name_len] = '\0';
  return true;
}

static bool normalize_absolute_path(const char *path, char *out, size_t cap) {
  if (path == NULL || path[0] != '/' || cap < 2) { return false; }
  size_t out_len = 1;
  out[0] = '/';
  out[1] = '\0';

  const char *p = path;
  while (*p != '\0') {
    while (*p == '/') {
      ++p;
    }
    if (*p == '\0') { break; }

    const char *start = p;
    while (*p != '\0' && *p != '/') {
      ++p;
    }
    size_t len = (size_t)(p - start);
    if (len == 1 && start[0] == '.') { continue; }
    if (len == 2 && start[0] == '.' && start[1] == '.') {
      if (out_len > 1) {
        while (out_len > 1 && out[out_len - 1] != '/') {
          --out_len;
        }
        if (out_len > 1) { --out_len; }
        out[out_len] = '\0';
      }
      continue;
    }

    if (out_len != 1) {
      if (out_len + 1 >= cap) { return false; }
      out[out_len++] = '/';
    }
    if (out_len + len >= cap) { return false; }
    kmemcpy(out + out_len, start, len);
    out_len += len;
    out[out_len] = '\0';
  }
  return true;
}

static bool combine_symlink_path(char *out, size_t cap, const char *parent, const char *target, const char *rest) {
  char raw[256];
  size_t pos = 0;
  if (cap == 0 || target[0] == '\0') { return false; }
  if (target[0] == '/') {
    for (const char *p = target; *p != '\0'; ++p) {
      if (pos + 1 >= sizeof(raw)) { return false; }
      raw[pos++] = *p;
    }
  } else {
    for (const char *p = parent; *p != '\0'; ++p) {
      if (pos + 1 >= sizeof(raw)) { return false; }
      raw[pos++] = *p;
    }
    if (!(pos == 1 && raw[0] == '/')) {
      if (pos + 1 >= sizeof(raw)) { return false; }
      raw[pos++] = '/';
    }
    for (const char *p = target; *p != '\0'; ++p) {
      if (pos + 1 >= sizeof(raw)) { return false; }
      raw[pos++] = *p;
    }
  }
  if (rest != NULL && rest[0] != '\0') {
    if (pos == 0 || raw[pos - 1] != '/') {
      if (pos + 1 >= sizeof(raw)) { return false; }
      raw[pos++] = '/';
    }
    for (const char *p = rest; *p != '\0'; ++p) {
      if (pos + 1 >= sizeof(raw)) { return false; }
      raw[pos++] = *p;
    }
  }
  raw[pos] = '\0';
  return normalize_absolute_path(raw, out, cap);
}

static bool add_dirent(struct ext2_fs *fs, struct ext2_node *dir, const char *name, uint32_t ino, uint8_t type) {
  uint8_t name_len = (uint8_t)kstrlen(name);
  uint16_t need = rec_len_for(name_len);
  uint8_t block[4096];
  if (fs->block_size > sizeof(block) || ext2plus_hidden_name(fs, dir, name, name_len)) { return false; }
  uint32_t blocks = (dir->size + fs->block_size - 1) / fs->block_size;
  for (uint32_t step = 0; step < blocks; ++step) {
    uint32_t bi = step == 0 ? blocks - 1 : step - 1;
    uint32_t disk_block = 0;
    if (!ext2_file_block(fs, dir, bi, &disk_block) || disk_block == 0 || !ext2_read_block(fs, disk_block, block)) { continue; }
    struct spore_ext2_dir_slot slot;
    int slot_result = spore_ext2_find_dir_slot(block, fs->block_size, need, &slot);
    if (slot_result < 0) { return false; }
    if (slot_result > 0) {
      uint32_t off = slot.offset;
      uint16_t rec_len = slot.rec_len;
      uint16_t used = slot.used;
        if (used == 0) {
          write_dir_record(block + off, ino, rec_len, name_len, type, name);
        } else {
          block[off + 4] = (uint8_t)used;
          block[off + 5] = (uint8_t)(used >> 8);
          write_dir_record(block + off + used, ino, (uint16_t)(rec_len - used), name_len, type, name);
        }
        uint32_t now = ext2_now_sec();
        dir->mtime = now;
        dir->ctime = now;
        bool ok = ext2_write_block(fs, disk_block, block) && ext2_write_inode(fs, dir);
        if (ok) { child_cache_store(fs, dir->ino, name, name_len, ino); }
        return ok;
    }
  }

  uint32_t new_block = 0;
  if (ext2_file_block_for_write(fs, dir, blocks, &new_block, true) != 0 || !ext2_read_block(fs, new_block, block)) {
    return false;
  }
  kmemset(block, 0, fs->block_size);
  write_dir_record(block, ino, (uint16_t)fs->block_size, name_len, type, name);
  dir->size += fs->block_size;
  uint32_t now = ext2_now_sec();
  dir->mtime = now;
  dir->ctime = now;
  bool ok = ext2_write_block(fs, new_block, block) && ext2_write_inode(fs, dir);
  if (ok) { child_cache_store(fs, dir->ino, name, name_len, ino); }
  return ok;
}

static bool remove_dirent(struct ext2_fs *fs, struct ext2_node *dir, const char *name, uint32_t *ino_out) {
  uint8_t block[4096];
  if (fs->block_size > sizeof(block)) { return false; }
  uint32_t blocks = (dir->size + fs->block_size - 1) / fs->block_size;
  for (uint32_t bi = 0; bi < blocks; ++bi) {
    uint32_t disk_block = 0;
    if (!ext2_file_block(fs, dir, bi, &disk_block) || disk_block == 0 || !ext2_read_block(fs, disk_block, block)) { continue; }
    struct spore_ext2_dir_match match;
    int match_result = spore_ext2_find_dir_name(block, fs->block_size, (const uint8_t *)name, kstrlen(name), &match);
    if (match_result < 0) { return false; }
    if (match_result > 0) {
        uint32_t off = match.offset;
        if (ino_out != NULL) { *ino_out = match.ino; }
        if (match.prev_offset != UINT32_MAX) {
          uint16_t prev_rec_len = (uint16_t)block[match.prev_offset + 4] | ((uint16_t)block[match.prev_offset + 5] << 8);
          uint16_t merged = (uint16_t)(prev_rec_len + match.rec_len);
          block[match.prev_offset + 4] = (uint8_t)merged;
          block[match.prev_offset + 5] = (uint8_t)(merged >> 8);
        } else {
          block[off] = block[off + 1] = block[off + 2] = block[off + 3] = 0;
        }
        uint32_t now = ext2_now_sec();
        dir->mtime = now;
        dir->ctime = now;
        bool ok = ext2_write_block(fs, disk_block, block) && ext2_write_inode(fs, dir);
        if (ok) { ext2_dir_cache_remove_name(fs, dir->ino, name); }
        return ok;
    }
  }
  return false;
}

static bool ext2_readlink_node(struct ext2_fs *fs, const struct ext2_node *node, char *out, size_t cap,
                               size_t *len_out) {
  if (!ext2_is_symlink(node) || cap == 0) { return false; }
  uint32_t len = node->size;
  if ((size_t)len >= cap) { len = (uint32_t)(cap - 1); }
  uint32_t got = 0;
  if (node->sectors_count == 0 && node->size <= sizeof(node->blocks)) {
    kmemcpy(out, node->blocks, len);
    got = len;
  } else if (!ext2_read_file(fs, node, 0, out, len, &got) || got != len) {
    return false;
  }
  out[len] = '\0';
  if (len_out != NULL) { *len_out = len; }
  return true;
}

static bool ext2_lookup_inner(struct ext2_fs *fs, const char *path, struct ext2_node *out, bool follow_final,
                              unsigned depth) {
  if (path == NULL || path[0] != '/' || depth > SYMLINK_DEPTH_MAX) { return false; }
  struct ext2_node cur;
  if (!ext2_read_inode(fs, EXT2_ROOT_INO, &cur)) { return false; }
  char parent_path[256] = "/";
  const char *p = path;
  while (*p == '/') {
    ++p;
  }
  if (*p == '\0') {
    *out = cur;
    return true;
  }
  while (*p != '\0') {
    const char *start = p;
    while (*p != '\0' && *p != '/') {
      ++p;
    }
    size_t len = (size_t)(p - start);
    if (len == 0) { return false; }
    struct ext2_node next;
    if (!lookup_child(fs, &cur, start, len, &next)) { return false; }
    while (*p == '/') {
      ++p;
    }
    bool final = *p == '\0';
    if (ext2_is_symlink(&next) && (!final || follow_final)) {
      char target[256];
      char combined[256];
      if (!ext2_readlink_node(fs, &next, target, sizeof(target), NULL) ||
          !combine_symlink_path(combined, sizeof(combined), parent_path, target, p)) {
        return false;
      }
      return ext2_lookup_inner(fs, combined, out, follow_final, depth + 1);
    }
    cur = next;
    if (!final && !append_path_component(parent_path, sizeof(parent_path), start, len)) { return false; }
  }
  *out = cur;
  return true;
}

bool ext2_lookup(struct ext2_fs *fs, const char *path, struct ext2_node *out) {
  ++ext2_stat_counters.lookup_count;
  return ext2_lookup_inner(fs, path, out, true, 0);
}

bool ext2_lstat(struct ext2_fs *fs, const char *path, struct ext2_node *out) {
  ++ext2_stat_counters.lstat_count;
  return ext2_lookup_inner(fs, path, out, false, 0);
}

bool ext2_create(struct ext2_fs *fs, const char *path, bool dir, uint16_t mode, struct ext2_node *out) {
  char parent_path[256];
  char name[EXT2_NAME_MAX + 1];
  if (!split_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name))) { return false; }
  struct ext2_node parent;
  if (!ext2_lookup(fs, parent_path, &parent) || !ext2_is_dir(&parent)) { return false; }
  struct ext2_node existing;
  if (lookup_child(fs, &parent, name, kstrlen(name), &existing)) {
    if (out != NULL) { *out = existing; }
    return true;
  }
  uint32_t ino = 0;
  if (!ext2_alloc_inode(fs, &ino)) { return false; }
  struct ext2_node node = {
    .ino = ino,
    .mode = (uint16_t)((dir ? EXT2_S_IFDIR : EXT2_S_IFREG) | (mode & 07777u)),
    .links_count = (uint16_t)(dir ? 2 : 1),
    .uid = 0,
    .gid = 0,
  };
  uint32_t now = ext2_now_sec();
  node.atime = now;
  node.ctime = now;
  node.mtime = now;
  if (dir) {
    uint32_t block = 0;
    uint8_t buf[4096];
    if (fs->block_size > sizeof(buf) || ext2_alloc_block(fs, &block) != 0) {
      (void)ext2_free_inode(fs, ino);
      return false;
    }
    node.blocks[0] = block;
    node.size = fs->block_size;
    kmemset(buf, 0, fs->block_size);
    write_dir_record(buf, ino, rec_len_for(1), 1, EXT2_FT_DIR, ".");
    write_dir_record(buf + rec_len_for(1), parent.ino, (uint16_t)(fs->block_size - rec_len_for(1)), 2, EXT2_FT_DIR,
                     "..");
    if (!ext2_write_block(fs, block, buf)) {
      node.blocks[0] = 0;
      node.size = 0;
      (void)ext2_free_block(fs, block);
      (void)ext2_free_inode(fs, ino);
      return false;
    }
  }
  if (!ext2_write_inode(fs, &node) || !add_dirent(fs, &parent, name, ino, dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE)) {
    (void)ext2_truncate(fs, &node, 0);
    (void)ext2_free_inode(fs, ino);
    return false;
  }
  if (dir) {
    if (parent.links_count != UINT16_MAX) { ++parent.links_count; }
    parent.mtime = now;
    parent.ctime = now;
    (void)ext2_write_inode(fs, &parent);
    (void)ext2_adjust_group_used_dirs(fs, ino, 1);
  }
  if (out != NULL) { *out = node; }
  return true;
}

static struct ext2_tmpfile_ref *tmpfile_ref(struct ext2_fs *fs, uint32_t ino) {
  for (size_t i = 0; i < fs->tmpfile_count; ++i) {
    if (fs->tmpfiles[i].ino == ino) { return &fs->tmpfiles[i]; }
  }
  return NULL;
}

bool ext2_tmpfile(struct ext2_fs *fs, uint16_t mode, uint32_t uid, uint32_t gid, bool linkable, struct ext2_node *out) {
  uint32_t ino;
  if (fs->write == NULL || uid > UINT16_MAX || gid > UINT16_MAX ||
      fs->tmpfile_count == sizeof(fs->tmpfiles) / sizeof(fs->tmpfiles[0]) || !ext2_alloc_inode(fs, &ino)) {
    return false;
  }
  uint32_t now = ext2_now_sec();
  struct ext2_node node = {
    .ino = ino, .mode = EXT2_S_IFREG | mode, .uid = uid, .gid = gid, .atime = now, .ctime = now, .mtime = now};
  if (!ext2_write_inode(fs, &node)) {
    (void)ext2_free_inode(fs, ino);
    return false;
  }
  fs->tmpfiles[fs->tmpfile_count++] = (struct ext2_tmpfile_ref){.ino = ino, .refs = 1, .linkable = linkable};
  *out = node;
  return true;
}

void ext2_tmpfile_retain(struct ext2_fs *fs, uint32_t ino) {
  struct ext2_tmpfile_ref *ref = tmpfile_ref(fs, ino);
  if (ref != NULL) { ++ref->refs; }
}

bool ext2_tmpfile_release(struct ext2_fs *fs, uint32_t ino) {
  struct ext2_tmpfile_ref *ref = tmpfile_ref(fs, ino);
  if (ref == NULL || --ref->refs != 0) { return false; }
  *ref = fs->tmpfiles[--fs->tmpfile_count];
  struct ext2_node node;
  return ext2_read_inode(fs, ino, &node) && node.links_count == 0 && ext2_truncate(fs, &node, 0) && ext2_free_inode(fs, ino);
}

bool ext2_link(struct ext2_fs *fs, const char *old_path, const char *new_path) {
  struct ext2_node node;
  return ext2_lookup(fs, old_path, &node) && ext2_link_inode(fs, node.ino, new_path);
}

bool ext2_link_inode(struct ext2_fs *fs, uint32_t ino, const char *new_path) {
  char new_parent_path[256];
  char new_name[EXT2_NAME_MAX + 1];
  if (!split_parent_path(new_path, new_parent_path, sizeof(new_parent_path), new_name, sizeof(new_name))) {
    return false;
  }
  struct ext2_node node;
  struct ext2_node parent;
  struct ext2_node existing;
  if (!ext2_read_inode(fs, ino, &node) || ext2_is_dir(&node) || !ext2_lookup(fs, new_parent_path, &parent) ||
      !ext2_is_dir(&parent) || lookup_child(fs, &parent, new_name, kstrlen(new_name), &existing)) {
    return false;
  }
  struct ext2_tmpfile_ref *ref = tmpfile_ref(fs, ino);
  if (node.links_count == UINT16_MAX || (node.links_count == 0 && (ref == NULL || !ref->linkable))) { return false; }
  ++node.links_count;
  node.ctime = ext2_now_sec();
  if (!ext2_write_inode(fs, &node)) { return false; }
  if (!add_dirent(fs, &parent, new_name, ino, dirent_type_for_node(&node))) {
    --node.links_count;
    (void)ext2_write_inode(fs, &node);
    return false;
  }
  if (ref != NULL) { ref->linkable = false; }
  return true;
}

bool ext2_symlink(struct ext2_fs *fs, const char *target, const char *link_path) {
  char parent_path[256];
  char name[EXT2_NAME_MAX + 1];
  if (target == NULL || target[0] == '\0' ||
      !split_parent_path(link_path, parent_path, sizeof(parent_path), name, sizeof(name))) {
    return false;
  }
  struct ext2_node parent;
  struct ext2_node existing;
  if (!ext2_lookup(fs, parent_path, &parent) || !ext2_is_dir(&parent) ||
      lookup_child(fs, &parent, name, kstrlen(name), &existing)) {
    return false;
  }
  uint32_t ino = 0;
  if (!ext2_alloc_inode(fs, &ino)) { return false; }
  struct ext2_node node = {
    .ino = ino,
    .mode = (uint16_t)(EXT2_S_IFLNK | 0777),
    .links_count = 1,
    .uid = 0,
    .gid = 0,
  };
  uint32_t now = ext2_now_sec();
  node.atime = now;
  node.ctime = now;
  node.mtime = now;
  size_t target_len = kstrlen(target);
  bool wrote_target = false;
  if (target_len <= sizeof(node.blocks)) {
    kmemcpy(node.blocks, target, target_len);
    node.size = (uint32_t)target_len;
    wrote_target = ext2_write_inode(fs, &node);
  } else {
    int64_t wrote = ext2_write_file(fs, &node, 0, target, target_len);
    wrote_target = wrote >= 0 && (uint64_t)wrote == target_len;
  }
  if (!wrote_target || !add_dirent(fs, &parent, name, ino, EXT2_FT_SYMLINK)) {
    (void)ext2_truncate(fs, &node, 0);
    (void)ext2_free_inode(fs, ino);
    return false;
  }
  return true;
}

bool ext2_readlink(struct ext2_fs *fs, const char *path, char *out, size_t cap, size_t *len_out) {
  struct ext2_node node;
  return ext2_lstat(fs, path, &node) && ext2_readlink_node(fs, &node, out, cap, len_out);
}

bool ext2_chmod_node(struct ext2_fs *fs, const struct ext2_node *node, uint32_t mode) {
  if (node == NULL) { return false; }
  struct ext2_node copy;
  if (!ext2_read_inode(fs, node->ino, &copy)) { return false; }
  copy.mode = (uint16_t)((copy.mode & 0170000u) | (mode & 07777u));
  copy.ctime = ext2_now_sec();
  return ext2_write_inode(fs, &copy);
}

bool ext2_chmod(struct ext2_fs *fs, const char *path, uint32_t mode) {
  struct ext2_node node;
  return ext2_lookup(fs, path, &node) && ext2_chmod_node(fs, &node, mode);
}

bool ext2_chown_node(struct ext2_fs *fs, const struct ext2_node *node, uint32_t uid, uint32_t gid) {
  if (node == NULL || uid > UINT16_MAX || gid > UINT16_MAX) { return false; }
  struct ext2_node copy;
  if (!ext2_read_inode(fs, node->ino, &copy)) { return false; }
  copy.uid = (uint16_t)uid;
  copy.gid = (uint16_t)gid;
  copy.ctime = ext2_now_sec();
  return ext2_write_inode(fs, &copy);
}

bool ext2_chown(struct ext2_fs *fs, const char *path, uint32_t uid, uint32_t gid) {
  struct ext2_node node;
  return ext2_lookup(fs, path, &node) && ext2_chown_node(fs, &node, uid, gid);
}

int ext2_remove(struct ext2_fs *fs, const char *path) {
  char parent_path[256];
  char name[EXT2_NAME_MAX + 1];
  if (!split_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name))) { return -EXT2_ENOENT; }
  struct ext2_node parent;
  struct ext2_node node;
  if (!ext2_lookup(fs, parent_path, &parent) || !lookup_child(fs, &parent, name, kstrlen(name), &node)) {
    return -EXT2_ENOENT;
  }
  if (ext2_is_dir(&node)) {
    uint64_t cursor = 0;
    int rc = read_next_dirent(fs, &node, &cursor, NULL);
    if (rc < 0) { return rc; }
    if (rc > 0) { return -EXT2_ENOTEMPTY; }
  }
  if (!remove_dirent(fs, &parent, name, NULL)) { return -EXT2_EIO; }
  if (!ext2_is_dir(&node) && (node.links_count > 1 || tmpfile_ref(fs, node.ino) != NULL)) {
    --node.links_count;
    node.ctime = ext2_now_sec();
    return ext2_write_inode(fs, &node) ? 0 : -EXT2_EIO;
  }
  (void)ext2_truncate(fs, &node, 0);
  if (ext2_is_dir(&node)) {
    if (parent.links_count > 2) { --parent.links_count; }
    parent.mtime = ext2_now_sec();
    parent.ctime = parent.mtime;
    (void)ext2_write_inode(fs, &parent);
    (void)ext2_adjust_group_used_dirs(fs, node.ino, -1);
  }
  (void)ext2_free_inode(fs, node.ino);
  return 0;
}

bool ext2_rename(struct ext2_fs *fs, const char *old_path, const char *new_path) {
  char old_parent_path[256];
  char old_name[EXT2_NAME_MAX + 1];
  char new_parent_path[256];
  char new_name[EXT2_NAME_MAX + 1];
  if (!split_parent_path(old_path, old_parent_path, sizeof(old_parent_path), old_name, sizeof(old_name)) ||
      !split_parent_path(new_path, new_parent_path, sizeof(new_parent_path), new_name, sizeof(new_name))) {
    return false;
  }
  struct ext2_node old_parent;
  struct ext2_node new_parent;
  struct ext2_node node;
  if (!ext2_lookup(fs, old_parent_path, &old_parent) || !ext2_lookup(fs, new_parent_path, &new_parent) ||
      !lookup_child(fs, &old_parent, old_name, kstrlen(old_name), &node)) {
    return false;
  }
  if (name_equals(old_parent_path, kstrlen(old_parent_path), new_parent_path) &&
      name_equals(old_name, kstrlen(old_name), new_name)) {
    return true;
  }
  struct ext2_node existing;
  if (lookup_child(fs, &new_parent, new_name, kstrlen(new_name), &existing)) {
    if ((ext2_is_dir(&node) && !ext2_is_dir(&existing)) || (!ext2_is_dir(&node) && ext2_is_dir(&existing))) {
      return false;
    }
    if (!ext2_unlink(fs, new_path)) { return false; }
    if (!ext2_read_inode(fs, new_parent.ino, &new_parent)) { return false; }
  }
  if (!add_dirent(fs, &new_parent, new_name, node.ino, dirent_type_for_node(&node))) { return false; }
  node.ctime = ext2_now_sec();
  (void)ext2_write_inode(fs, &node);
  if (remove_dirent(fs, &old_parent, old_name, NULL)) { return true; }
  (void)ext2_unlink(fs, new_path);
  return false;
}
