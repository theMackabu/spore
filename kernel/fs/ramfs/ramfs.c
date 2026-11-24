#include "internal.h"

#include "lib/mem.h"
uint64_t ramfs_now_sec;

void ramfs_set_now(uint64_t epoch_sec) {
  ramfs_now_sec = epoch_sec;
}

static bool streq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) { return false; }
  }
  return *a == '\0' && *b == '\0';
}

static bool copy_name(char *dst, const char *src) {
  size_t i = 0;
  for (; i < RAMFS_NAME_MAX && src[i] != '\0'; ++i) {
    dst[i] = src[i];
  }
  if (src[i] != '\0') { return false; }
  dst[i] = '\0';
  return true;
}

static const char *last_component(const char *path) {
  const char *name = path;
  for (const char *p = path; *p != '\0'; ++p) {
    if (*p == '/' && p[1] != '\0') { name = p + 1; }
  }
  return name;
}

static int alloc_node(struct ramfs *fs) {
  for (int i = 0; i < RAMFS_MAX_NODES; ++i) {
    if (!fs->nodes[i].used) {
      kmemset(&fs->nodes[i], 0, sizeof(fs->nodes[i]));
      fs->nodes[i].used = true;
      fs->nodes[i].parent = -1;
      fs->nodes[i].link_target = -1;
      fs->nodes[i].links_count = 1;
      fs->nodes[i].first_page = -1;
      fs->nodes[i].ino = fs->next_ino++;
      fs->nodes[i].atime = ramfs_now_sec;
      fs->nodes[i].ctime = ramfs_now_sec;
      fs->nodes[i].mtime = ramfs_now_sec;
      return i;
    }
  }
  return -1;
}

static void free_node(struct ramfs *fs, int node) {
  ramfs_free_node_pages(fs, node);
  fs->nodes[node] = (struct ramfs_mem_node){.parent = -1, .first_page = -1};
}

int ramfs_find_child(const struct ramfs *fs, int parent, const char *name) {
  for (int i = 0; i < RAMFS_MAX_NODES; ++i) {
    if (fs->nodes[i].used && fs->nodes[i].parent == parent && streq(fs->nodes[i].name, name)) { return i; }
  }
  return -1;
}

static int lookup_entry(const struct ramfs *fs, const char *path) {
  if (path == NULL || path[0] != '/') { return -1; }
  if (streq(path, "/")) { return 0; }
  int current = 0;
  const char *p = path + 1;
  char component[RAMFS_NAME_MAX + 1];
  while (*p != '\0') {
    size_t len = 0;
    while (p[len] != '\0' && p[len] != '/') {
      if (len >= RAMFS_NAME_MAX) { return -1; }
      component[len] = p[len];
      ++len;
    }
    component[len] = '\0';
    current = ramfs_find_child(fs, current, component);
    if (current < 0) { return -1; }
    p += len;
    if (*p == '/') { ++p; }
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
  if (path == NULL || path[0] != '/' || streq(path, "/")) { return false; }
  const char *last = last_component(path);
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
  if (parent < 0 || !fs->nodes[parent].is_dir || ramfs_find_child(fs, parent, name) >= 0) { return -1; }
  int index = alloc_node(fs);
  if (index < 0) { return -1; }
  if (!copy_name(fs->nodes[index].name, name)) {
    free_node(fs, index);
    return -1;
  }
  fs->nodes[index].parent = parent;
  fs->nodes[index].is_dir = is_dir;
  fs->nodes[index].writable = writable;
  fs->nodes[index].mount = fs->nodes[parent].mount;
  fs->nodes[index].mode = is_dir ? 0040777u : 0100666u;
  fs->nodes[index].uid = 0;
  fs->nodes[index].gid = 0;
  fs->nodes[parent].mtime = ramfs_now_sec;
  fs->nodes[parent].ctime = ramfs_now_sec;
  return index;
}

void ramfs_init(struct ramfs *fs, const struct spore_boot_module *modules, uint32_t module_count,
                uint64_t hhdm_offset) {
  kmemset(fs, 0, sizeof(*fs));
  ramfs_reset_pages(hhdm_offset);
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
  for (size_t i = first; i < RAMFS_MAX_NODES; ++i) {
    if (!fs->nodes[i].used || fs->nodes[i].parent != dir_index || (int)i == dir_index) { continue; }
    const struct ramfs_mem_node *node = &fs->nodes[inode_index(fs, (int)i)];
    const char *name = fs->nodes[i].name;
    out->name_len = (uint8_t)kstrlen(name);
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
  if (node->is_dir) {
    for (int i = 0; i < RAMFS_MAX_NODES; ++i) {
      if (fs->nodes[i].used && fs->nodes[i].parent == index) { return -39; }
    }
  }
  if (entry == index) {
    node->parent = -1;
    node->name[0] = 0;
  } else {
    free_node(fs, entry);
  }
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
    if (!copy_name(node->name, name)) { return false; }
    node->parent = parent;
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

bool ramfs_rename(struct ramfs *fs, const char *old_path, const char *new_path) {
  int index = lookup_entry(fs, old_path);
  int parent;
  const char *name;
  if (index <= 0 || !split_parent(fs, new_path, &parent, &name)) { return false; }
  int existing = lookup_entry(fs, new_path);
  if (inode_index(fs, existing) == inode_index(fs, index)) { return true; }
  if (existing >= 0 && !ramfs_unlink(fs, new_path)) { return false; }
  if (!copy_name(fs->nodes[index].name, name)) { return false; }
  fs->nodes[index].parent = parent;
  fs->nodes[index].ctime = ramfs_now_sec;
  return true;
}
