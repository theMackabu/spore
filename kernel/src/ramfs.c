#include "ramfs.h"

#include "mem.h"

static const char motd[] = "welcome to spore\n";

static bool streq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) { return false; }
  }
  return *a == '\0' && *b == '\0';
}

static void copy_name(char *dst, const char *src) {
  size_t i = 0;
  for (; i < RAMFS_NAME_MAX && src[i] != '\0'; ++i) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
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
      fs->nodes[i].ino = fs->next_ino++;
      return i;
    }
  }
  return -1;
}

static int find_child(const struct ramfs *fs, int parent, const char *name) {
  for (int i = 0; i < RAMFS_MAX_NODES; ++i) {
    if (fs->nodes[i].used && fs->nodes[i].parent == parent && streq(fs->nodes[i].name, name)) { return i; }
  }
  return -1;
}

static int lookup_index(const struct ramfs *fs, const char *path) {
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
    current = find_child(fs, current, component);
    if (current < 0) { return -1; }
    p += len;
    if (*p == '/') { ++p; }
  }
  return current;
}

static bool split_parent(const struct ramfs *fs, const char *path, int *parent, const char **name) {
  if (path == NULL || path[0] != '/' || streq(path, "/")) { return false; }
  const char *last = last_component(path);
  if (*last == '\0') { return false; }
  char parent_path[128];
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

static int add_node(struct ramfs *fs, int parent, const char *name, bool is_dir, bool writable) {
  if (parent < 0 || !fs->nodes[parent].is_dir || find_child(fs, parent, name) >= 0) { return -1; }
  int index = alloc_node(fs);
  if (index < 0) { return -1; }
  fs->nodes[index].parent = parent;
  fs->nodes[index].is_dir = is_dir;
  fs->nodes[index].writable = writable;
  copy_name(fs->nodes[index].name, name);
  return index;
}

static bool add_ro_file(struct ramfs *fs, const char *path, const void *data, uint64_t size) {
  int parent;
  const char *name;
  if (!split_parent(fs, path, &parent, &name)) { return false; }
  int index = add_node(fs, parent, name, false, false);
  if (index < 0) { return false; }
  fs->nodes[index].ro_data = data;
  fs->nodes[index].size = size;
  return true;
}

void ramfs_init(struct ramfs *fs, const struct spore_boot_module *modules, uint32_t module_count,
                uint64_t hhdm_offset) {
  kmemset(fs, 0, sizeof(*fs));
  fs->next_ino = 1;

  int root = alloc_node(fs);
  fs->nodes[root].is_dir = true;
  fs->nodes[root].parent = root;
  copy_name(fs->nodes[root].name, "");

  (void)add_node(fs, root, "bin", true, true);
  (void)add_node(fs, root, "demos", true, true);
  (void)add_node(fs, root, "etc", true, true);
  (void)add_node(fs, root, "tmp", true, true);
  (void)add_ro_file(fs, "/etc/motd", motd, sizeof(motd) - 1);

  if (modules == NULL) { return; }
  for (uint32_t i = 0; i < module_count; ++i) {
    const struct spore_boot_module *file = &modules[i];
    const char *path = file->path;
    if (path != NULL && path[0] == '/') {
      (void)add_ro_file(fs, path, (const void *)(uintptr_t)(hhdm_offset + file->phys_addr), file->size);
    }
  }
}

bool ramfs_lookup(const struct ramfs *fs, const char *path, struct ramfs_file *out) {
  int index = lookup_index(fs, path);
  if (index < 0 || fs->nodes[index].is_dir) { return false; }
  out->path = path;
  out->data = fs->nodes[index].writable ? fs->nodes[index].data : fs->nodes[index].ro_data;
  out->size = fs->nodes[index].size;
  return true;
}

bool ramfs_refresh_node(struct ramfs *fs, int index, struct ramfs_node *out) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used) { return false; }
  struct ramfs_mem_node *node = &fs->nodes[index];
  *out = (struct ramfs_node){
    .fs = fs,
    .index = index,
    .name = index == 0 ? "/" : node->name,
    .data = node->writable ? node->data : node->ro_data,
    .size = node->size,
    .ino = node->ino,
    .is_dir = node->is_dir,
  };
  return true;
}

bool ramfs_lookup_node(const struct ramfs *fs_const, const char *path, struct ramfs_node *out) {
  struct ramfs *fs = (struct ramfs *)fs_const;
  int index = lookup_index(fs, path);
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_dirent(const struct ramfs *fs, int dir_index, size_t index, struct ramfs_dirent *out) {
  if (dir_index < 0 || dir_index >= RAMFS_MAX_NODES || !fs->nodes[dir_index].used || !fs->nodes[dir_index].is_dir) {
    return false;
  }
  size_t seen = 0;
  for (int i = 0; i < RAMFS_MAX_NODES; ++i) {
    if (fs->nodes[i].used && fs->nodes[i].parent == dir_index && i != dir_index) {
      if (seen == index) {
        out->name = fs->nodes[i].name;
        out->ino = fs->nodes[i].ino;
        out->is_dir = fs->nodes[i].is_dir;
        return true;
      }
      ++seen;
    }
  }
  return false;
}

bool ramfs_root_dirent(size_t index, struct ramfs_dirent *out) {
  static const struct ramfs_dirent entries[] = {
    {.name = "bin", .ino = 2, .is_dir = true},   {.name = "demos", .ino = 3, .is_dir = true},
    {.name = "etc", .ino = 4, .is_dir = true},   {.name = "tmp", .ino = 5, .is_dir = true},
    {.name = "init", .ino = 7, .is_dir = false},
  };
  if (index >= sizeof(entries) / sizeof(entries[0])) { return false; }
  *out = entries[index];
  return true;
}

bool ramfs_mkdir(struct ramfs *fs, const char *path) {
  int parent;
  const char *name;
  return split_parent(fs, path, &parent, &name) && add_node(fs, parent, name, true, true) >= 0;
}

bool ramfs_create(struct ramfs *fs, const char *path, struct ramfs_node *out) {
  int existing = lookup_index(fs, path);
  if (existing >= 0) { return ramfs_refresh_node(fs, existing, out); }
  int parent;
  const char *name;
  if (!split_parent(fs, path, &parent, &name)) { return false; }
  int index = add_node(fs, parent, name, false, true);
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_truncate(struct ramfs *fs, int index, uint64_t size) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || fs->nodes[index].is_dir ||
      !fs->nodes[index].writable || size > RAMFS_FILE_CAP) {
    return false;
  }
  if (size > fs->nodes[index].size) {
    kmemset(fs->nodes[index].data + fs->nodes[index].size, 0, (size_t)(size - fs->nodes[index].size));
  }
  fs->nodes[index].size = size;
  return true;
}

bool ramfs_unlink(struct ramfs *fs, const char *path) {
  int index = lookup_index(fs, path);
  if (index <= 0 || !fs->nodes[index].used) { return false; }
  if (fs->nodes[index].is_dir) {
    for (int i = 0; i < RAMFS_MAX_NODES; ++i) {
      if (fs->nodes[i].used && fs->nodes[i].parent == index) { return false; }
    }
  }
  fs->nodes[index].used = false;
  return true;
}

bool ramfs_rename(struct ramfs *fs, const char *old_path, const char *new_path) {
  int index = lookup_index(fs, old_path);
  int parent;
  const char *name;
  if (index <= 0 || !split_parent(fs, new_path, &parent, &name)) { return false; }
  int existing = lookup_index(fs, new_path);
  if (existing > 0) { fs->nodes[existing].used = false; }
  fs->nodes[index].parent = parent;
  copy_name(fs->nodes[index].name, name);
  return true;
}

uint64_t ramfs_read(struct ramfs *fs, int index, uint64_t off, void *dst, uint64_t len) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || fs->nodes[index].is_dir ||
      off >= fs->nodes[index].size) {
    return 0;
  }
  uint64_t n = fs->nodes[index].size - off;
  if (n > len) { n = len; }
  const uint8_t *src = fs->nodes[index].writable ? fs->nodes[index].data : fs->nodes[index].ro_data;
  kmemcpy(dst, src + off, (size_t)n);
  return n;
}

int64_t ramfs_write(struct ramfs *fs, int index, uint64_t off, const void *src, uint64_t len) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || fs->nodes[index].is_dir ||
      !fs->nodes[index].writable || off + len > RAMFS_FILE_CAP) {
    return -1;
  }
  kmemcpy(fs->nodes[index].data + off, src, (size_t)len);
  if (off + len > fs->nodes[index].size) { fs->nodes[index].size = off + len; }
  return (int64_t)len;
}
