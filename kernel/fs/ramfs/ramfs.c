#include "internal.h"

#include "lib/mem.h"
#include "lib/kstr.h"

uint64_t ramfs_now_sec;

void ramfs_set_now(uint64_t epoch_sec) {
  ramfs_now_sec = epoch_sec;
}

static int alloc_node(struct ramfs *fs) {
  int i;
  if (fs->first_free) {
    i = fs->first_free - 1;
    fs->first_free = fs->nodes[i].hash_next;
  } else {
    if (fs->next_unused == RAMFS_MAX_NODES) { return -1; }
    i = fs->next_unused++;
  }
  fs->nodes[i] = (struct ramfs_mem_node){
    .used = true, .parent = -1, .link_target = -1, .links_count = 1, .first_page = -1,
    .ino = fs->next_ino++, .atime = ramfs_now_sec, .ctime = ramfs_now_sec, .mtime = ramfs_now_sec,
  };
  return i;
}

static void free_node(struct ramfs *fs, int i) {
  ramfs_free_node_pages(fs, i);
  fs->nodes[i].used = false;
  fs->nodes[i].hash_next = fs->first_free;
  fs->first_free = i + 1;
}

static unsigned name_bucket(int parent, const char *name, size_t len) {
  uint32_t hash = (uint32_t)parent ^ 2166136261u;
  for (size_t i = 0; i < len; ++i) { hash = (hash ^ (uint8_t)name[i]) * 16777619u; }
  return hash % RAMFS_NAME_BUCKETS;
}

static int find_component(const struct ramfs *fs, int parent, const char *name, size_t len) {
  unsigned bucket = name_bucket(parent, name, len);
  for (int slot = fs->names[bucket]; slot; slot = fs->nodes[slot - 1].hash_next) {
    const struct ramfs_mem_node *n = &fs->nodes[slot - 1];
    if (n->parent == parent && n->name_len == len && !kmemcmp(n->name, name, len)) { return slot - 1; }
  }
  return -1;
}

int ramfs_find_child(const struct ramfs *fs, int parent, const char *name) {
  return find_component(fs, parent, name, kstrlen(name));
}

static void attach_name(struct ramfs *fs, int i, int parent, const char *name, size_t len) {
  struct ramfs_mem_node *n = &fs->nodes[i];
  n->parent = parent;
  n->name_len = (uint8_t)len;
  kmemcpy(n->name, name, len);
  n->name[len] = 0;
  unsigned bucket = name_bucket(parent, name, len);
  n->hash_next = fs->names[bucket];
  fs->names[bucket] = i + 1;
  ++fs->nodes[parent].children;
  fs->nodes[parent].ctime = fs->nodes[parent].mtime = ramfs_now_sec;
}

static void detach_name(struct ramfs *fs, int i) {
  struct ramfs_mem_node *n = &fs->nodes[i];
  int *link = &fs->names[name_bucket(n->parent, n->name, n->name_len)];
  while (*link != i + 1) { link = &fs->nodes[*link - 1].hash_next; }
  *link = n->hash_next;
  --fs->nodes[n->parent].children;
  fs->nodes[n->parent].ctime = fs->nodes[n->parent].mtime = ramfs_now_sec;
  n->parent = -1;
  n->name_len = 0;
  n->name[0] = 0;
}

static int lookup_entry(const struct ramfs *fs, const char *path) {
  if (path == NULL || path[0] != '/') { return -1; }
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
  return entry >= 0 && fs->nodes[entry].link_target >= 0 ? fs->nodes[entry].link_target : entry;
}

static int lookup_index(const struct ramfs *fs, const char *path) {
  return inode_index(fs, lookup_entry(fs, path));
}

static bool split_parent(const struct ramfs *fs, const char *path, int *parent, const char **name) {
  if (path == NULL || path[0] != '/' || str_eq(path, "/")) { return false; }
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
  if (p < 0 || !fs->nodes[p].is_dir) { return false; }
  *parent = p;
  *name = last;
  return true;
}

int ramfs_add_node(struct ramfs *fs, int parent, const char *name, bool is_dir, bool writable) {
  size_t len = kstrlen(name);
  if (parent < 0 || !fs->nodes[parent].is_dir || !len || len > RAMFS_NAME_MAX ||
      find_component(fs, parent, name, len) >= 0) { return -1; }
  int index = alloc_node(fs);
  if (index < 0) { return -1; }
  attach_name(fs, index, parent, name, len);
  fs->nodes[index].is_dir = is_dir;
  fs->nodes[index].writable = writable;
  fs->nodes[index].mount = fs->nodes[parent].mount;
  fs->nodes[index].mode = is_dir ? 0040777u : 0100666u;
  fs->nodes[index].uid = 0;
  fs->nodes[index].gid = 0;
  return index;
}

void ramfs_init(struct ramfs *fs, const struct spore_boot_module *modules, uint32_t module_count,
                uint64_t hhdm_offset) {
  kmemset(fs, 0, sizeof(*fs));
  ramfs_reset_pages();
  fs->hhdm_offset = hhdm_offset;
  fs->next_ino = 1;

  int root = alloc_node(fs);
  fs->nodes[root].is_dir = true;
  fs->nodes[root].mode = 0040777u;
  fs->nodes[root].parent = root;
  fs->nodes[root].mount = RAMFS_MOUNT_RAM0;

  ramfs_populate(fs, root, modules, module_count);
}

bool ramfs_refresh_node(struct ramfs *fs, int index, struct ramfs_node *out) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used) { return false; }
  struct ramfs_mem_node *node = &fs->nodes[index];
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
  if (fs == NULL || index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used) { return false; }
  ++fs->nodes[index].refs;
  return true;
}

void ramfs_release_node(struct ramfs *fs, int index) {
  if (fs == NULL || index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || fs->nodes[index].refs == 0) {
    return;
  }
  --fs->nodes[index].refs;
  if (fs->nodes[index].refs == 0 && fs->nodes[index].links_count == 0 && index != 0) { free_node(fs, index); }
}

bool ramfs_set_sealable(struct ramfs *fs, int index, bool allow_sealing) {
  if (fs == NULL || index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || fs->nodes[index].is_dir ||
      fs->nodes[index].device != RAMFS_DEV_NONE) {
    return false;
  }
  fs->nodes[index].sealable = true;
  fs->nodes[index].seals = allow_sealing ? 0 : RAMFS_F_SEAL_SEAL;
  fs->nodes[index].ctime = ramfs_now_sec;
  return true;
}

int ramfs_add_seals(struct ramfs *fs, int index, uint32_t seals) {
  if (fs == NULL || index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || !fs->nodes[index].sealable ||
      (seals & ~RAMFS_F_SEAL_MASK) != 0) {
    return -22;
  }
  struct ramfs_mem_node *node = &fs->nodes[index];
  if ((node->seals & RAMFS_F_SEAL_SEAL) != 0) { return -1; }
  if ((seals & RAMFS_F_SEAL_WRITE) != 0 && node->shared_write_mappings != 0) { return -16; }
  node->seals |= seals;
  node->ctime = ramfs_now_sec;
  return 0;
}

int ramfs_get_seals(struct ramfs *fs, int index) {
  if (fs == NULL || index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || !fs->nodes[index].sealable) {
    return -22;
  }
  return (int)fs->nodes[index].seals;
}

bool ramfs_write_sealed(struct ramfs *fs, int index) {
  if (fs == NULL || index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used) { return false; }
  return (fs->nodes[index].seals & (RAMFS_F_SEAL_WRITE | RAMFS_F_SEAL_FUTURE_WRITE)) != 0;
}

bool ramfs_truncate_sealed(struct ramfs *fs, int index, uint64_t size) {
  if (fs == NULL || index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used) { return false; }
  uint32_t seals = fs->nodes[index].seals;
  return (size < fs->nodes[index].size && (seals & RAMFS_F_SEAL_SHRINK) != 0) ||
         (size > fs->nodes[index].size && (seals & RAMFS_F_SEAL_GROW) != 0);
}

void ramfs_note_shared_writable_mapping(struct ramfs *fs, int index, bool add) {
  if (fs == NULL || index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used) { return; }
  if (add) {
    ++fs->nodes[index].shared_write_mappings;
  } else if (fs->nodes[index].shared_write_mappings != 0) {
    --fs->nodes[index].shared_write_mappings;
  }
}

bool ramfs_lookup_node(const struct ramfs *fs_const, const char *path, struct ramfs_node *out) {
  struct ramfs *fs = (struct ramfs *)fs_const;
  int index = lookup_index(fs, path);
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_next_dirent(const struct ramfs *fs, int dir_index, uint64_t *cursor, struct vfs_dirent *out) {
  if (cursor == NULL || dir_index < 0 || dir_index >= RAMFS_MAX_NODES || !fs->nodes[dir_index].used ||
      !fs->nodes[dir_index].is_dir) {
    return false;
  }
  if (*cursor >= RAMFS_MAX_NODES) { return false; }
  size_t first = (size_t)*cursor;
  for (size_t i = first; i < (size_t)fs->next_unused; ++i) {
    if (!fs->nodes[i].used || fs->nodes[i].parent != dir_index || (int)i == dir_index) { continue; }
    const struct ramfs_mem_node *node = &fs->nodes[inode_index(fs, (int)i)];
    const char *name = fs->nodes[i].name;
    out->name_len = fs->nodes[i].name_len;
    kmemcpy(out->name, name, (size_t)out->name_len + 1);
    out->ino = node->ino;
    out->type = (uint8_t)((node->mode & 0170000u) >> 12);
    *cursor = i + 1;
    return true;
  }
  *cursor = RAMFS_MAX_NODES;
  return false;
}

bool ramfs_mkdir(struct ramfs *fs, const char *path, uint16_t mode) {
  int parent;
  const char *name;
  int index = split_parent(fs, path, &parent, &name) ? ramfs_add_node(fs, parent, name, true, true) : -1;
  if (index < 0) { return false; }
  fs->nodes[index].mode = (uint16_t)(0040000u | (mode & 07777u));
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
  fs->nodes[index].mode = (uint16_t)(0100000u | (mode & 07777u));
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_mkfifo(struct ramfs *fs, const char *path, uint16_t mode, struct ramfs_node *out) {
  if (lookup_index(fs, path) >= 0) { return false; }
  int parent;
  const char *name;
  if (!split_parent(fs, path, &parent, &name)) { return false; }
  int index = ramfs_add_node(fs, parent, name, false, true);
  if (index < 0) { return false; }
  fs->nodes[index].mode = (uint16_t)(0010000u | (mode & 0777u));
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_mksock(struct ramfs *fs, const char *path, uint16_t mode, struct ramfs_node *out) {
  if (lookup_index(fs, path) >= 0) { return false; }
  int parent;
  const char *name;
  if (!split_parent(fs, path, &parent, &name)) { return false; }
  int index = ramfs_add_node(fs, parent, name, false, true);
  if (index < 0) { return false; }
  fs->nodes[index].mode = (uint16_t)(0140000u | (mode & 0777u));
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_chmod_node(struct ramfs *fs, int index, uint16_t mode) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used) { return false; }
  fs->nodes[index].mode = (fs->nodes[index].mode & 0170000u) | (mode & 07777u);
  fs->nodes[index].ctime = ramfs_now_sec;
  return true;
}

bool ramfs_chown_node(struct ramfs *fs, int index, uint32_t uid, uint32_t gid) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used) { return false; }
  fs->nodes[index].uid = uid;
  fs->nodes[index].gid = gid;
  fs->nodes[index].ctime = ramfs_now_sec;
  return true;
}

bool ramfs_tmpfile(const struct ramfs_node *dir, uint16_t mode, uint32_t uid, uint32_t gid, bool linkable,
                   struct ramfs_node *out) {
  struct ramfs *fs = dir->fs;
  int index = alloc_node(fs);
  if (index < 0) { return false; }
  struct ramfs_mem_node *node = &fs->nodes[index];
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
  struct ramfs_mem_node *node = &fs->nodes[index];
  if (node->children) { return -39; }
  detach_name(fs, entry);
  if (entry != index) { free_node(fs, entry); }
  --node->links_count;
  node->ctime = ramfs_now_sec;
  if (node->links_count == 0 && node->refs == 0) { free_node(fs, index); }
  return 0;
}

bool ramfs_link_node(struct ramfs *fs, int index, const char *new_path) {
  if (index <= 0 || index >= RAMFS_MAX_NODES) { return false; }
  struct ramfs_mem_node *node = &fs->nodes[index];
  if (!node->used || node->is_dir || node->device != RAMFS_DEV_NONE || (!node->links_count && !node->linkable)) {
    return false;
  }
  int parent;
  const char *name;
  if (!split_parent(fs, new_path, &parent, &name) || fs->nodes[parent].mount != node->mount ||
      ramfs_find_child(fs, parent, name) >= 0) {
    return false;
  }
  if (node->parent < 0) {
    attach_name(fs, index, parent, name, kstrlen(name));
  } else {
    int entry = ramfs_add_node(fs, parent, name, false, false);
    if (entry < 0) { return false; }
    fs->nodes[entry].link_target = index;
  }
  ++node->links_count;
  node->linkable = false;
  node->ctime = ramfs_now_sec;
  fs->nodes[parent].ctime = fs->nodes[parent].mtime = ramfs_now_sec;
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
  struct ramfs_mem_node *node = &fs->nodes[index];
  if (fs->nodes[parent].mount != node->mount) { return -18; }
  int existing = ramfs_find_child(fs, parent, name);
  if (existing >= 0) {
    if (noreplace) { return -17; }
    int target = inode_index(fs, existing);
    if (target == index) { return 0; }
    if (node->is_dir != fs->nodes[target].is_dir) { return node->is_dir ? -20 : -21; }
    if (fs->nodes[target].children) { return -39; }
  }
  if (node->is_dir) {
    for (int at = parent; at > 0; at = fs->nodes[at].parent) {
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
