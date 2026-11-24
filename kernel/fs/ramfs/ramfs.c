#include "internal.h"

#include "lib/mem.h"
#include "lib/kstr.h"

uint64_t ramfs_now_sec;

void ramfs_set_now(uint64_t epoch_sec) {
  ramfs_now_sec = epoch_sec;
}

static uint32_t name_hash(int parent, const char *name, size_t len) {
  uint32_t hash = (uint32_t)parent ^ 2166136261u;
  for (size_t i = 0; i < len; ++i) { hash = (hash ^ (uint8_t)name[i]) * 16777619u; }
  return hash;
}

static int find_component(const struct ramfs *fs, int parent, const char *name, size_t len) {
  uint32_t hash = name_hash(parent, name, len);
  unsigned bucket = hash & (fs->table->capacity - 1);
  for (int slot = fs->table->slots[bucket].bucket; slot; slot = ramfs_inode_at(fs, slot - 1)->hash_next) {
    const struct ramfs_mem_node *n = ramfs_inode_at(fs, slot - 1);
    if (n->name_hash == hash && n->parent == parent && n->name_len == len && !kmemcmp(n->name, name, len)) { return slot - 1; }
  }
  return -1;
}

int ramfs_find_child(const struct ramfs *fs, int parent, const char *name) {
  return find_component(fs, parent, name, kstrlen(name));
}

static void link_child(struct ramfs *fs, struct ramfs_mem_node *dir, int i) {
  int next = 0;
  if (i < dir->last_child) {
    next = dir->first_child;
    for (int probe = i + 1; next < i; ++probe) {
      const struct ramfs_mem_node *candidate = ramfs_inode_at(fs, probe);
      if (candidate && candidate->parent == ramfs_inode_at(fs, i)->parent) { next = probe; break; }
      next = ramfs_inode_at(fs, next)->next_sibling;
    }
  }
  int prev = next ? ramfs_inode_at(fs, next)->prev_sibling : dir->last_child;
  struct ramfs_mem_node *n = ramfs_inode_at(fs, i);
  n->prev_sibling = prev;
  n->next_sibling = next;
  if (prev) { ramfs_inode_at(fs, prev)->next_sibling = i; }
  else { dir->first_child = i; }
  if (next) { ramfs_inode_at(fs, next)->prev_sibling = i; }
  else { dir->last_child = i; }
}

static void attach_name(struct ramfs *fs, int i, int parent, const char *name, size_t len) {
  struct ramfs_mem_node *n = ramfs_inode_at(fs, i);
  n->parent = parent;
  n->name_len = (uint8_t)len;
  kmemcpy(n->name, name, len);
  n->name[len] = 0;
  uint32_t hash = name_hash(parent, name, len);
  unsigned bucket = hash & (fs->table->capacity - 1);
  n->name_hash = hash;
  n->hash_next = fs->table->slots[bucket].bucket;
  fs->table->slots[bucket].bucket = i + 1;
  struct ramfs_mem_node *dir = ramfs_inode_at(fs, parent);
  link_child(fs, dir, i);
  if (n->is_dir) { ++dir->links_count; }
  dir->ctime = dir->mtime = ramfs_now_sec;
}

static void detach_name(struct ramfs *fs, int i) {
  struct ramfs_mem_node *n = ramfs_inode_at(fs, i);
  int *link = &fs->table->slots[n->name_hash & (fs->table->capacity - 1)].bucket;
  while (*link != i + 1) { link = &ramfs_inode_at(fs, *link - 1)->hash_next; }
  *link = n->hash_next;
  struct ramfs_mem_node *dir = ramfs_inode_at(fs, n->parent);
  if (n->prev_sibling) { ramfs_inode_at(fs, n->prev_sibling)->next_sibling = n->next_sibling; }
  else { dir->first_child = n->next_sibling; }
  if (n->next_sibling) { ramfs_inode_at(fs, n->next_sibling)->prev_sibling = n->prev_sibling; }
  else { dir->last_child = n->prev_sibling; }
  if (n->is_dir) { --dir->links_count; }
  dir->ctime = dir->mtime = ramfs_now_sec;
  n->prev_sibling = n->next_sibling = 0;
  n->parent = -1;
  n->name_len = 0;
  n->name[0] = 0;
}

static int lookup_entry(const struct ramfs *fs, const char *path) {
  if (!fs || !fs->table || path == NULL || path[0] != '/') { return -1; }
  int current = 0;
  const char *p = path + 1;
  while (*p) {
    while (*p == '/') { ++p; }
    if (!*p) { break; }
    const char *name = p;
    while (*p && *p != '/') { ++p; }
    size_t len = (size_t)(p - name);
    if (len > RAMFS_NAME_MAX) { return -1; }
    current = find_component(fs, current, name, len);
    if (current < 0) { return -1; }
  }
  return current;
}

static int inode_index(const struct ramfs *fs, int entry) {
  return entry >= 0 && ramfs_inode_at(fs, entry)->link_target >= 0 ? ramfs_inode_at(fs, entry)->link_target : entry;
}

static int lookup_index(const struct ramfs *fs, const char *path) {
  return inode_index(fs, lookup_entry(fs, path));
}

static bool split_parent(const struct ramfs *fs, const char *path, int *parent, const char **name) {
  if (!fs || !fs->table || path == NULL || path[0] != '/' || str_eq(path, "/")) { return false; }
  const char *last = base_name(path);
  if (*last == '\0') { return false; }
  size_t name_len = kstrlen(last);
  if (name_len == 0 || name_len > RAMFS_NAME_MAX) { return false; }
  char parent_path[512];
  size_t parent_len = (size_t)(last - path);
  if (parent_len == 0) { parent_len = 1; }
  if (parent_len >= sizeof(parent_path)) { return false; }
  for (size_t i = 0; i < parent_len; ++i) {
    parent_path[i] = path[i];
  }
  if (parent_len > 1 && parent_path[parent_len - 1] == '/') { --parent_len; }
  parent_path[parent_len] = '\0';
  int p = lookup_index(fs, parent_path);
  if (p < 0 || !ramfs_inode_at(fs, p)->is_dir) { return false; }
  *parent = p;
  *name = last;
  return true;
}

int ramfs_add_node(struct ramfs *fs, int parent, const char *name, bool is_dir, bool writable) {
  size_t len = kstrlen(name);
  if (parent < 0 || !ramfs_inode_at(fs, parent)->is_dir || !len || len > RAMFS_NAME_MAX ||
      find_component(fs, parent, name, len) >= 0) { return -1; }
  int index = ramfs_alloc_inode(fs);
  if (index < 0) { return -1; }
  struct ramfs_mem_node *node = ramfs_inode_at(fs, index);
  node->is_dir = is_dir;
  node->links_count = is_dir ? 2 : 1;
  node->writable = writable;
  node->mount = ramfs_inode_at(fs, parent)->mount;
  node->mode = is_dir ? 0040777u : 0100666u;
  attach_name(fs, index, parent, name, len);
  return index;
}

bool ramfs_init(struct ramfs *fs, const struct spore_boot_module *modules, uint32_t module_count,
                uint64_t hhdm_offset) {
  ramfs_destroy(fs);
  fs->hhdm_offset = hhdm_offset;
  fs->next_ino = 1;

  int root = ramfs_alloc_inode(fs);
  if (root < 0) { ramfs_destroy(fs); return false; }
  ramfs_inode_at(fs, root)->links_count = 2;
  ramfs_inode_at(fs, root)->is_dir = true;
  ramfs_inode_at(fs, root)->mode = 0040777u;
  ramfs_inode_at(fs, root)->parent = root;
  ramfs_inode_at(fs, root)->mount = RAMFS_MOUNT_RAM0;

  if (!ramfs_populate(fs, root, modules, module_count)) { ramfs_destroy(fs); return false; }
  return true;
}

bool ramfs_refresh_node(struct ramfs *fs, int index, struct ramfs_node *out) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (!node) { return false; }
  *out = (struct ramfs_node){
    .fs = fs,
    .index = index,
    .name = index == 0 ? "/" : node->name,
    .size = node->size,
    .allocated_pages = node->ro_data ? (uint32_t)((node->size + RAMFS_PAGE_SIZE - 1) / RAMFS_PAGE_SIZE)
                                     : node->allocated_pages,
    .ino = node->ino,
    .is_dir = node->is_dir,
    .mount = node->mount,
    .device = node->device,
    .mode = node->mode,
    .uid = node->uid,
    .gid = node->gid,
    .links_count = node->links_count,
    .atime = node->atime,
    .ctime = node->ctime,
    .mtime = node->mtime,
  };
  return true;
}

bool ramfs_retain_node(struct ramfs *fs, int index) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (!node) { return false; }
  ++node->refs;
  return true;
}

void ramfs_release_node(struct ramfs *fs, int index) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (!node || node->refs == 0) {
    return;
  }
  --node->refs;
  if (node->refs == 0 && node->links_count == 0 && index != 0) { ramfs_free_inode(fs, index); }
}

bool ramfs_set_sealable(struct ramfs *fs, int index, bool allow_sealing) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (!node || node->is_dir ||
      node->device != RAMFS_DEV_NONE) {
    return false;
  }
  node->sealable = true;
  node->seals = allow_sealing ? 0 : RAMFS_F_SEAL_SEAL;
  node->ctime = ramfs_now_sec;
  return true;
}

int ramfs_add_seals(struct ramfs *fs, int index, uint32_t seals) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (!node || !node->sealable ||
      (seals & ~RAMFS_F_SEAL_MASK) != 0) {
    return -22;
  }
  if ((node->seals & RAMFS_F_SEAL_SEAL) != 0) { return -1; }
  if ((seals & RAMFS_F_SEAL_WRITE) != 0 && node->shared_write_mappings != 0) { return -16; }
  node->seals |= seals;
  node->ctime = ramfs_now_sec;
  return 0;
}

int ramfs_get_seals(struct ramfs *fs, int index) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (!node || !node->sealable) {
    return -22;
  }
  return (int)node->seals;
}

bool ramfs_write_sealed(struct ramfs *fs, int index) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (!node) { return false; }
  return (node->seals & (RAMFS_F_SEAL_WRITE | RAMFS_F_SEAL_FUTURE_WRITE)) != 0;
}

bool ramfs_truncate_sealed(struct ramfs *fs, int index, uint64_t size) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (!node) { return false; }
  uint32_t seals = node->seals;
  return (size < node->size && (seals & RAMFS_F_SEAL_SHRINK) != 0) ||
         (size > node->size && (seals & RAMFS_F_SEAL_GROW) != 0);
}

void ramfs_note_shared_writable_mapping(struct ramfs *fs, int index, bool add) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (!node) { return; }
  if (add) {
    ++node->shared_write_mappings;
  } else if (node->shared_write_mappings != 0) {
    --node->shared_write_mappings;
  }
}

bool ramfs_lookup_node(const struct ramfs *fs_const, const char *path, struct ramfs_node *out) {
  struct ramfs *fs = (struct ramfs *)fs_const;
  int index = lookup_index(fs, path);
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_next_dirent(const struct ramfs *fs, int dir_index, uint64_t *cursor, struct vfs_dirent *out) {
  struct ramfs_mem_node *directory = ramfs_get_inode(fs, dir_index);
  if (!cursor || !directory || !directory->is_dir || *cursor >= RAMFS_DIR_END) { return false; }
  int i = directory->first_child;
  if (*cursor && *cursor <= (uint64_t)fs->table->next_unused) {
    int last = (int)(*cursor - 1);
    const struct ramfs_mem_node *previous = ramfs_inode_at(fs, last);
    if (previous && previous->parent == dir_index && last != dir_index) { i = previous->next_sibling; }
  }
  while (i && (uint64_t)i < *cursor) { i = ramfs_inode_at(fs, i)->next_sibling; }
  if (!i) { *cursor = RAMFS_DIR_END; return false; }
  const struct ramfs_mem_node *entry = ramfs_inode_at(fs, i);
  const struct ramfs_mem_node *node = ramfs_inode_at(fs, inode_index(fs, i));
  out->name_len = entry->name_len;
  kmemcpy(out->name, entry->name, (size_t)out->name_len + 1);
  out->ino = node->ino;
  out->type = (uint8_t)((node->mode & 0170000u) >> 12);
  *cursor = (uint64_t)i + 1;
  return true;
}

bool ramfs_mkdir(struct ramfs *fs, const char *path, uint16_t mode) {
  int parent;
  const char *name;
  int index = split_parent(fs, path, &parent, &name) ? ramfs_add_node(fs, parent, name, true, true) : -1;
  if (index < 0) { return false; }
  ramfs_inode_at(fs, index)->mode = (uint16_t)(0040000u | (mode & 07777u));
  return true;
}

bool ramfs_create(struct ramfs *fs, const char *path, uint16_t mode, struct ramfs_node *out) {
  int existing = lookup_index(fs, path);
  if (existing >= 0) { return ramfs_refresh_node(fs, existing, out); }
  int parent;
  const char *name;
  if (!split_parent(fs, path, &parent, &name)) { return false; }
  int index = ramfs_add_node(fs, parent, name, false, true);
  if (index < 0) { return false; }
  ramfs_inode_at(fs, index)->mode = (uint16_t)(0100000u | (mode & 07777u));
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_mkfifo(struct ramfs *fs, const char *path, uint16_t mode, struct ramfs_node *out) {
  if (lookup_index(fs, path) >= 0) { return false; }
  int parent;
  const char *name;
  if (!split_parent(fs, path, &parent, &name)) { return false; }
  int index = ramfs_add_node(fs, parent, name, false, true);
  if (index < 0) { return false; }
  ramfs_inode_at(fs, index)->mode = (uint16_t)(0010000u | (mode & 0777u));
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_mksock(struct ramfs *fs, const char *path, uint16_t mode, struct ramfs_node *out) {
  if (lookup_index(fs, path) >= 0) { return false; }
  int parent;
  const char *name;
  if (!split_parent(fs, path, &parent, &name)) { return false; }
  int index = ramfs_add_node(fs, parent, name, false, true);
  if (index < 0) { return false; }
  ramfs_inode_at(fs, index)->mode = (uint16_t)(0140000u | (mode & 0777u));
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_set_times(struct ramfs *fs, int index, uint64_t atime, uint64_t mtime) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (!node) { return false; }
  node->atime = atime;
  node->mtime = mtime;
  node->ctime = ramfs_now_sec;
  return true;
}

bool ramfs_chmod_node(struct ramfs *fs, int index, uint16_t mode) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (!node) { return false; }
  node->mode = (node->mode & 0170000u) | (mode & 07777u);
  node->ctime = ramfs_now_sec;
  return true;
}

bool ramfs_chown_node(struct ramfs *fs, int index, uint32_t uid, uint32_t gid) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (!node) { return false; }
  node->uid = uid;
  node->gid = gid;
  node->ctime = ramfs_now_sec;
  return true;
}

bool ramfs_tmpfile(const struct ramfs_node *dir, uint16_t mode, uint32_t uid, uint32_t gid, bool linkable,
                   struct ramfs_node *out) {
  struct ramfs *fs = dir->fs;
  int index = ramfs_alloc_inode(fs);
  if (index < 0) { return false; }
  struct ramfs_mem_node *node = ramfs_inode_at(fs, index);
  node->mount = dir->mount;
  node->writable = true;
  node->mode = 0100000u | mode;
  node->uid = uid;
  node->gid = gid;
  node->refs = 1;
  node->links_count = 0;
  node->linkable = linkable;
  return ramfs_refresh_node(fs, index, out);
}

int ramfs_remove(struct ramfs *fs, const char *path) {
  int entry = lookup_entry(fs, path);
  if (entry <= 0) { return -2; }
  int index = inode_index(fs, entry);
  struct ramfs_mem_node *node = ramfs_inode_at(fs, index);
  if (node->first_child) { return -39; }
  detach_name(fs, entry);
  if (entry != index) { ramfs_free_inode(fs, entry); }
  if (node->is_dir) { node->links_count = 0; }
  else { --node->links_count; }
  node->ctime = ramfs_now_sec;
  if (node->links_count == 0 && node->refs == 0) { ramfs_free_inode(fs, index); }
  return 0;
}

bool ramfs_link_node(struct ramfs *fs, int index, const char *new_path) {
  struct ramfs_mem_node *node = ramfs_get_inode(fs, index);
  if (index == 0 || !node) { return false; }
  if (node->is_dir || node->device != RAMFS_DEV_NONE || (!node->links_count && !node->linkable)) {
    return false;
  }
  int parent;
  const char *name;
  if (!split_parent(fs, new_path, &parent, &name) || ramfs_inode_at(fs, parent)->mount != node->mount ||
      ramfs_find_child(fs, parent, name) >= 0) {
    return false;
  }
  if (node->parent < 0) {
    attach_name(fs, index, parent, name, kstrlen(name));
  } else {
    int entry = ramfs_add_node(fs, parent, name, false, false);
    if (entry < 0) { return false; }
    ramfs_inode_at(fs, entry)->link_target = index;
  }
  ++node->links_count;
  node->linkable = false;
  node->ctime = ramfs_now_sec;
  ramfs_inode_at(fs, parent)->ctime = ramfs_inode_at(fs, parent)->mtime = ramfs_now_sec;
  return true;
}

bool ramfs_link(struct ramfs *fs, const char *old_path, const char *new_path) {
  return ramfs_link_node(fs, lookup_index(fs, old_path), new_path);
}

int ramfs_renameat(struct ramfs *fs, const char *old_path, const char *new_path, bool noreplace) {
  int entry = lookup_entry(fs, old_path);
  int parent;
  const char *name;
  if (entry <= 0 || !split_parent(fs, new_path, &parent, &name)) { return -2; }
  int index = inode_index(fs, entry);
  struct ramfs_mem_node *node = ramfs_inode_at(fs, index);
  if (ramfs_inode_at(fs, parent)->mount != node->mount) { return -18; }
  int existing = ramfs_find_child(fs, parent, name);
  if (existing >= 0) {
    if (noreplace) { return -17; }
    int target = inode_index(fs, existing);
    if (target == index) { return 0; }
    if (node->is_dir != ramfs_inode_at(fs, target)->is_dir) { return node->is_dir ? -20 : -21; }
    if (ramfs_inode_at(fs, target)->first_child) { return -39; }
  }
  if (node->is_dir) {
    for (int at = parent; at > 0; at = ramfs_inode_at(fs, at)->parent) {
      if (at == index) { return -22; }
    }
  }
  if (existing >= 0) {
    int rc = ramfs_remove(fs, new_path);
    if (rc) { return rc; }
  }
  detach_name(fs, entry);
  attach_name(fs, entry, parent, name, kstrlen(name));
  node->ctime = ramfs_now_sec;
  return 0;
}
