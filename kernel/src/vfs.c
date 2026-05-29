#include "vfs.h"

#include "mem.h"
#include "mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

static struct ramfs *root_ramfs;
static struct ext2_fs *root_ext2;

enum {
  VFS_PAGE_SIZE = 4096,
  VFS_PAGE_CACHE_ENTRIES = 64,
  VFS_LOOKUP_CACHE_ENTRIES = 128,
  VFS_LOOKUP_CACHE_PATH_MAX = 255,
  VFS_READAHEAD_PAGES = 2,
};

struct vfs_page_cache_entry {
  bool valid;
  enum vfs_backend backend;
  uint64_t ino;
  uint64_t dev_id;
  uint64_t off;
  uint64_t age;
  uint8_t data[VFS_PAGE_SIZE];
};

struct vfs_lookup_cache_entry {
  bool valid;
  bool lstat;
  uint64_t age;
  char path[VFS_LOOKUP_CACHE_PATH_MAX + 1];
  struct vfs_node node;
};

static struct vfs_page_cache_entry page_cache[VFS_PAGE_CACHE_ENTRIES];
static struct vfs_lookup_cache_entry lookup_cache[VFS_LOOKUP_CACHE_ENTRIES];
static uint64_t page_cache_clock;
static uint64_t lookup_cache_clock;
static struct vfs_stats vfs_stat_counters;

bool cell_proc_exists(int pid);
int cell_proc_pid_at(size_t index);
uint32_t cell_proc_uid(int pid);
uint32_t cell_proc_gid(int pid);
uint64_t cell_realtime_seconds(void);

enum {
  BOOT_IMAGE_BYTES = 16 * 1024 * 1024,
  VFS_DEV_EXT2_ROOT = 0x0801,
  VFS_DEV_DEVFS = 0x0005,
  VFS_DEV_PROC = 0x0006,
  VFS_DEV_RAM0 = 0x0010,
  VFS_DEV_TMPFS = 0x0011,
  VFS_DEV_RUNFS = 0x0012,
};

static bool starts_with(const char *s, const char *prefix) {
  while (*prefix != '\0') {
    if (*s++ != *prefix++) { return false; }
  }
  return true;
}

static bool path_is_mount(const char *path, const char *mount) {
  if (!starts_with(path, mount)) { return false; }
  char next = path[kstrlen(mount)];
  return next == '\0' || next == '/';
}

static bool ramfs_route(const char *path) {
  return path_is_mount(path, "/dev") || path_is_mount(path, "/proc") || path_is_mount(path, "/tmp") ||
         path_is_mount(path, "/run");
}

static bool same_ramfs_route(const char *a, const char *b) {
  return (path_is_mount(a, "/dev") && path_is_mount(b, "/dev")) ||
         (path_is_mount(a, "/proc") && path_is_mount(b, "/proc")) ||
         (path_is_mount(a, "/tmp") && path_is_mount(b, "/tmp")) ||
         (path_is_mount(a, "/run") && path_is_mount(b, "/run"));
}

static bool ramfs_device_is_block(enum ramfs_device device) {
  return device == RAMFS_DEV_BLK_ROOT || device == RAMFS_DEV_BLK_BOOT;
}

static uint64_t make_rdev(uint64_t major, uint64_t minor) {
  return (major << 8) | minor;
}

static uint64_t ramfs_mount_dev_id(enum ramfs_mount mount) {
  switch (mount) {
  case RAMFS_MOUNT_DEV:
    return VFS_DEV_DEVFS;
  case RAMFS_MOUNT_PROC:
    return VFS_DEV_PROC;
  case RAMFS_MOUNT_TMP:
    return VFS_DEV_TMPFS;
  case RAMFS_MOUNT_RUN:
    return VFS_DEV_RUNFS;
  case RAMFS_MOUNT_RAM0:
    return VFS_DEV_RAM0;
  }
  return VFS_DEV_RAM0;
}

static uint64_t ramfs_device_rdev(enum ramfs_device device) {
  switch (device) {
  case RAMFS_DEV_NULL:
    return make_rdev(1, 3);
  case RAMFS_DEV_ZERO:
    return make_rdev(1, 5);
  case RAMFS_DEV_FULL:
    return make_rdev(1, 7);
  case RAMFS_DEV_RANDOM:
    return make_rdev(1, 8);
  case RAMFS_DEV_URANDOM:
    return make_rdev(1, 9);
  case RAMFS_DEV_CONSOLE:
    return make_rdev(5, 1);
  case RAMFS_DEV_TTY:
    return make_rdev(5, 0);
  case RAMFS_DEV_BLK_ROOT:
    return make_rdev(254, 0);
  case RAMFS_DEV_BLK_BOOT:
    return make_rdev(254, 1);
  default:
    return 0;
  }
}

static void update_time_sources(void) {
  uint64_t now = cell_realtime_seconds();
  ramfs_set_now(now);
  ext2_set_now((uint32_t)now);
}

static void from_ramfs(const struct ramfs_node *node, struct vfs_node *out) {
  uint16_t perms = node->mode == 0 ? (node->is_dir ? 0777u : 0666u) : (uint16_t)(node->mode & 07777u);
  uint16_t type = (uint16_t)(node->mode & 0170000u);
  uint16_t mode =
    type != 0 ? (uint16_t)(type | perms) : (node->is_dir ? (uint16_t)(0040000u | perms) : (uint16_t)(0100000u | perms));
  if (node->device != RAMFS_DEV_NONE) {
    if (ramfs_device_is_block(node->device)) {
      mode = (uint16_t)(0060000u | perms);
    } else if (node->mount == RAMFS_MOUNT_PROC) {
      mode = (uint16_t)(0100000u | perms);
    } else {
      mode = (uint16_t)(0020000u | perms);
    }
  }
  *out = (struct vfs_node){
    .backend = VFS_RAMFS,
    .ino = node->ino,
    .is_dir = node->is_dir,
    .writable = true,
    .device = node->device,
    .mode = mode,
    .links_count = 1,
    .uid = node->uid,
    .gid = node->gid,
    .dev_id = ramfs_mount_dev_id(node->mount),
    .rdev = ramfs_device_rdev(node->device),
    .atime = node->atime,
    .ctime = node->ctime,
    .mtime = node->mtime,
    .size = node->size,
    .ramfs = *node,
  };
}

static void from_ext2(const struct ext2_node *node, struct vfs_node *out) {
  *out = (struct vfs_node){
    .backend = VFS_EXT2,
    .ino = node->ino,
    .is_dir = ext2_is_dir(node),
    .writable = false,
    .device = RAMFS_DEV_NONE,
    .mode = node->mode,
    .links_count = node->links_count,
    .uid = node->uid,
    .gid = node->gid,
    .dev_id = VFS_DEV_EXT2_ROOT,
    .rdev = 0,
    .atime = node->atime,
    .ctime = node->ctime,
    .mtime = node->mtime,
    .size = node->size,
    .ext2 = *node,
  };
}

void vfs_init(struct ramfs *ramfs, struct ext2_fs *ext2, uint64_t hhdm_offset) {
  (void)hhdm_offset;
  root_ramfs = ramfs;
  root_ext2 = ext2;
  update_time_sources();
}

static bool streq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) { return false; }
  }
  return *a == '\0' && *b == '\0';
}

struct vfs_stats vfs_get_stats(void) {
  return vfs_stat_counters;
}

static bool copy_cache_path(char *dst, const char *path) {
  size_t len = kstrlen(path);
  if (len > VFS_LOOKUP_CACHE_PATH_MAX) { return false; }
  kmemcpy(dst, path, len + 1);
  return true;
}

static bool lookup_cache_get(const char *path, bool lstat, struct vfs_node *out) {
  for (size_t i = 0; i < VFS_LOOKUP_CACHE_ENTRIES; ++i) {
    if (lookup_cache[i].valid && lookup_cache[i].lstat == lstat && streq(lookup_cache[i].path, path)) {
      lookup_cache[i].age = ++lookup_cache_clock;
      *out = lookup_cache[i].node;
      ++vfs_stat_counters.lookup_cache_hits;
      return true;
    }
  }
  ++vfs_stat_counters.lookup_cache_misses;
  return false;
}

static struct vfs_lookup_cache_entry *lookup_cache_victim(void) {
  struct vfs_lookup_cache_entry *victim = &lookup_cache[0];
  for (size_t i = 0; i < VFS_LOOKUP_CACHE_ENTRIES; ++i) {
    if (!lookup_cache[i].valid) { return &lookup_cache[i]; }
    if (lookup_cache[i].age < victim->age) { victim = &lookup_cache[i]; }
  }
  return victim;
}

static void lookup_cache_put(const char *path, bool lstat, const struct vfs_node *node) {
  if (node == NULL || node->backend == VFS_PROC) { return; }
  struct vfs_lookup_cache_entry *entry = lookup_cache_victim();
  char path_copy[VFS_LOOKUP_CACHE_PATH_MAX + 1];
  if (!copy_cache_path(path_copy, path)) { return; }
  entry->valid = false;
  entry->lstat = lstat;
  kmemcpy(entry->path, path_copy, kstrlen(path_copy) + 1);
  entry->node = *node;
  entry->age = ++lookup_cache_clock;
  entry->valid = true;
}

static void lookup_cache_invalidate_all(void) {
  bool any = false;
  for (size_t i = 0; i < VFS_LOOKUP_CACHE_ENTRIES; ++i) {
    if (lookup_cache[i].valid) {
      lookup_cache[i].valid = false;
      any = true;
    }
  }
  if (any) { ++vfs_stat_counters.lookup_cache_invalidations; }
}

static bool node_cacheable(const struct vfs_node *node) {
  return node != NULL && !node->is_dir && node->device == RAMFS_DEV_NONE &&
         (node->backend == VFS_RAMFS || node->backend == VFS_EXT2);
}

static bool cache_key_eq(const struct vfs_page_cache_entry *entry, const struct vfs_node *node, uint64_t off) {
  return entry->valid && entry->backend == node->backend && entry->ino == node->ino && entry->dev_id == node->dev_id &&
         entry->off == off;
}

static struct vfs_page_cache_entry *page_cache_find(const struct vfs_node *node, uint64_t off) {
  for (size_t i = 0; i < VFS_PAGE_CACHE_ENTRIES; ++i) {
    if (cache_key_eq(&page_cache[i], node, off)) {
      page_cache[i].age = ++page_cache_clock;
      return &page_cache[i];
    }
  }
  return NULL;
}

static struct vfs_page_cache_entry *page_cache_victim(void) {
  struct vfs_page_cache_entry *victim = &page_cache[0];
  for (size_t i = 0; i < VFS_PAGE_CACHE_ENTRIES; ++i) {
    if (!page_cache[i].valid) { return &page_cache[i]; }
    if (page_cache[i].age < victim->age) { victim = &page_cache[i]; }
  }
  return victim;
}

static bool page_cache_load(const struct vfs_node *node, uint64_t off) {
  if (!node_cacheable(node)) { return false; }
  off &= ~(uint64_t)(VFS_PAGE_SIZE - 1);
  if (page_cache_find(node, off) != NULL) { return true; }
  ++vfs_stat_counters.page_cache_loads;
  struct vfs_page_cache_entry *entry = page_cache_victim();
  entry->valid = false;
  entry->backend = node->backend;
  entry->ino = node->ino;
  entry->dev_id = node->dev_id;
  entry->off = off;
  entry->age = ++page_cache_clock;
  kmemset(entry->data, 0, sizeof(entry->data));
  uint64_t readable = 0;
  if (off < node->size) {
    readable = node->size - off;
    if (readable > VFS_PAGE_SIZE) { readable = VFS_PAGE_SIZE; }
  }
  if (readable != 0 && vfs_read(node, off, entry->data, readable) != readable) { return false; }
  entry->valid = true;
  return true;
}

bool vfs_read_page_cached(const struct vfs_node *node, uint64_t off, void *dst) {
  if (dst == NULL || !node_cacheable(node)) { return false; }
  off &= ~(uint64_t)(VFS_PAGE_SIZE - 1);
  if (page_cache_find(node, off) != NULL) {
    ++vfs_stat_counters.page_cache_hits;
  } else {
    ++vfs_stat_counters.page_cache_misses;
  }
  if (!page_cache_load(node, off)) { return false; }
  for (uint64_t i = 1; i <= VFS_READAHEAD_PAGES; ++i) {
    uint64_t ahead = off + i * VFS_PAGE_SIZE;
    if (ahead >= node->size) { break; }
    (void)page_cache_load(node, ahead);
  }
  struct vfs_page_cache_entry *entry = page_cache_find(node, off);
  if (entry == NULL) { return false; }
  kmemcpy(dst, entry->data, VFS_PAGE_SIZE);
  return true;
}

void vfs_page_cache_invalidate(const struct vfs_node *node) {
  if (node == NULL) { return; }
  bool any = false;
  for (size_t i = 0; i < VFS_PAGE_CACHE_ENTRIES; ++i) {
    if (page_cache[i].valid && page_cache[i].backend == node->backend && page_cache[i].ino == node->ino &&
        page_cache[i].dev_id == node->dev_id) {
      page_cache[i].valid = false;
      any = true;
    }
  }
  if (any) { ++vfs_stat_counters.page_cache_invalidations; }
}

static bool parse_uint_component(const char **p, int *out) {
  int value = 0;
  const char *s = *p;
  if (*s < '0' || *s > '9') { return false; }
  while (*s >= '0' && *s <= '9') {
    value = value * 10 + (*s - '0');
    ++s;
  }
  *p = s;
  *out = value;
  return true;
}

static bool proc_kind_for_name(const char *name, enum ramfs_device *device) {
  if (streq(name, "stat")) {
    *device = RAMFS_DEV_PROC_PID_STAT;
  } else if (streq(name, "status")) {
    *device = RAMFS_DEV_PROC_PID_STATUS;
  } else if (streq(name, "cmdline")) {
    *device = RAMFS_DEV_PROC_PID_CMDLINE;
  } else if (streq(name, "statm")) {
    *device = RAMFS_DEV_PROC_PID_STATM;
  } else if (streq(name, "comm")) {
    *device = RAMFS_DEV_PROC_PID_COMM;
  } else if (streq(name, "mounts")) {
    *device = RAMFS_DEV_PROC_PID_MOUNTS;
  } else if (streq(name, "cwd")) {
    *device = RAMFS_DEV_PROC_PID_CWD;
  } else if (streq(name, "exe")) {
    *device = RAMFS_DEV_PROC_PID_EXE;
  } else {
    return false;
  }
  return true;
}

static bool proc_pid_file_node(int pid, enum ramfs_device device, struct vfs_node *out) {
  uint64_t now = cell_realtime_seconds();
  *out = (struct vfs_node){
    .backend = VFS_PROC,
    .ino = 110000u + (uint64_t)pid * 16u + (uint64_t)device,
    .is_dir = false,
    .writable = false,
    .device = device,
    .mode = 0100444u,
    .links_count = 1,
    .uid = cell_proc_uid(pid),
    .gid = cell_proc_gid(pid),
    .dev_id = VFS_DEV_PROC,
    .atime = now,
    .ctime = now,
    .mtime = now,
    .proc_pid = pid,
  };
  return true;
}

static void utoa_dec(uint64_t value, char *dst, size_t cap) {
  char tmp[32];
  size_t n = 0;
  if (cap == 0) { return; }
  if (value == 0) { tmp[n++] = '0'; }
  while (value != 0 && n < sizeof(tmp)) {
    tmp[n++] = (char)('0' + (value % 10));
    value /= 10;
  }
  size_t out = 0;
  while (n > 0 && out + 1 < cap) {
    dst[out++] = tmp[--n];
  }
  dst[out] = '\0';
}

static bool lookup_proc_dynamic(const char *path, struct vfs_node *out) {
  if (!path_is_mount(path, "/proc")) { return false; }
  const char *p = path + 5;
  if (*p != '/') { return false; }
  ++p;
  int pid = 0;
  if (!parse_uint_component(&p, &pid) || !cell_proc_exists(pid)) { return false; }
  if (*p == '\0') {
    uint64_t now = cell_realtime_seconds();
    *out = (struct vfs_node){
      .backend = VFS_PROC,
      .ino = 100000u + (uint64_t)pid,
      .is_dir = true,
      .writable = false,
      .device = RAMFS_DEV_NONE,
      .mode = 0040555u,
      .links_count = 1,
      .uid = cell_proc_uid(pid),
      .gid = cell_proc_gid(pid),
      .dev_id = VFS_DEV_PROC,
      .atime = now,
      .ctime = now,
      .mtime = now,
      .proc_pid = pid,
    };
    return true;
  }
  if (*p != '/') { return false; }
  ++p;
  if (streq(p, "task")) {
    uint64_t now = cell_realtime_seconds();
    *out = (struct vfs_node){
      .backend = VFS_PROC,
      .ino = 120000u + (uint64_t)pid,
      .is_dir = true,
      .writable = false,
      .device = RAMFS_DEV_NONE,
      .mode = 0040555u,
      .links_count = 1,
      .uid = cell_proc_uid(pid),
      .gid = cell_proc_gid(pid),
      .dev_id = VFS_DEV_PROC,
      .atime = now,
      .ctime = now,
      .mtime = now,
      .proc_pid = -pid,
    };
    return true;
  }
  if (starts_with(p, "task/")) {
    p += 5;
    int tid = 0;
    if (!parse_uint_component(&p, &tid) || tid != pid || !cell_proc_exists(tid)) { return false; }
    if (*p == '\0') {
      uint64_t now = cell_realtime_seconds();
      *out = (struct vfs_node){
        .backend = VFS_PROC,
        .ino = 130000u + (uint64_t)pid,
        .is_dir = true,
        .writable = false,
        .device = RAMFS_DEV_NONE,
        .mode = 0040555u,
        .links_count = 1,
        .uid = cell_proc_uid(pid),
        .gid = cell_proc_gid(pid),
        .dev_id = VFS_DEV_PROC,
        .atime = now,
        .ctime = now,
        .mtime = now,
        .proc_pid = pid,
      };
      return true;
    }
    if (*p != '/') { return false; }
    ++p;
  }
  enum ramfs_device device;
  if (!proc_kind_for_name(p, &device)) { return false; }
  return proc_pid_file_node(pid, device, out);
}

static bool lookup_ramfs(const char *path, struct vfs_node *out) {
  struct ramfs_node node;
  if (root_ramfs == NULL || !ramfs_lookup_node(root_ramfs, path, &node)) { return false; }
  from_ramfs(&node, out);
  return true;
}

bool vfs_lookup(const char *path, struct vfs_node *out) {
  ++vfs_stat_counters.lookup_count;
  if (lookup_cache_get(path, false, out)) { return true; }
  if (ramfs_route(path)) {
    if (lookup_proc_dynamic(path, out)) { return true; }
    if (lookup_ramfs(path, out)) {
      lookup_cache_put(path, false, out);
      return true;
    }
  }
  if (root_ext2 != NULL) {
    struct ext2_node node;
    if (ext2_lookup(root_ext2, path, &node)) {
      from_ext2(&node, out);
      lookup_cache_put(path, false, out);
      return true;
    }
  }
  if (!lookup_ramfs(path, out)) { return false; }
  lookup_cache_put(path, false, out);
  return true;
}

bool vfs_lstat(const char *path, struct vfs_node *out) {
  ++vfs_stat_counters.lstat_count;
  if (lookup_cache_get(path, true, out)) { return true; }
  if (ramfs_route(path)) {
    if (lookup_proc_dynamic(path, out)) { return true; }
    if (lookup_ramfs(path, out)) {
      lookup_cache_put(path, true, out);
      return true;
    }
  }
  if (root_ext2 != NULL) {
    struct ext2_node node;
    if (ext2_lstat(root_ext2, path, &node)) {
      from_ext2(&node, out);
      lookup_cache_put(path, true, out);
      return true;
    }
  }
  if (!lookup_ramfs(path, out)) { return false; }
  lookup_cache_put(path, true, out);
  return true;
}

bool vfs_mkdir(const char *path) {
  update_time_sources();
  bool ok = false;
  if (root_ext2 != NULL && !ramfs_route(path)) {
    struct ext2_node node;
    ok = ext2_create(root_ext2, path, true, &node);
  } else {
    ok = root_ramfs != NULL && ramfs_mkdir(root_ramfs, path);
  }
  if (ok) { lookup_cache_invalidate_all(); }
  return ok;
}

bool vfs_create(const char *path, struct vfs_node *out) {
  update_time_sources();
  if (root_ext2 != NULL && !ramfs_route(path)) {
    struct ext2_node node;
    if (!ext2_create(root_ext2, path, false, &node)) { return false; }
    from_ext2(&node, out);
    out->writable = true;
    lookup_cache_invalidate_all();
    return true;
  }
  struct ramfs_node node;
  if (root_ramfs == NULL || !ramfs_create(root_ramfs, path, &node)) { return false; }
  from_ramfs(&node, out);
  lookup_cache_invalidate_all();
  return true;
}

bool vfs_mkfifo(const char *path, uint32_t mode, struct vfs_node *out) {
  update_time_sources();
  struct ramfs_node node;
  if (root_ramfs == NULL || !ramfs_route(path) || !ramfs_mkfifo(root_ramfs, path, (uint16_t)mode, &node)) {
    return false;
  }
  from_ramfs(&node, out);
  lookup_cache_invalidate_all();
  return true;
}

bool vfs_mksock(const char *path, uint32_t mode, struct vfs_node *out) {
  update_time_sources();
  struct ramfs_node node;
  if (root_ramfs == NULL || !ramfs_route(path) || !ramfs_mksock(root_ramfs, path, (uint16_t)mode, &node)) {
    return false;
  }
  from_ramfs(&node, out);
  lookup_cache_invalidate_all();
  return true;
}

bool vfs_truncate(const struct vfs_node *node, uint64_t size) {
  update_time_sources();
  vfs_page_cache_invalidate(node);
  bool ok = false;
  if (node->backend == VFS_EXT2) {
    struct ext2_node ext = node->ext2;
    ok = ext2_truncate(root_ext2, &ext, size);
  } else if (node->backend == VFS_RAMFS) {
    ok = ramfs_truncate(node->ramfs.fs, node->ramfs.index, size);
  }
  if (ok) { lookup_cache_invalidate_all(); }
  return ok;
}

bool vfs_unlink(const char *path) {
  update_time_sources();
  bool ok = false;
  if (root_ext2 != NULL && !ramfs_route(path)) {
    ok = ext2_unlink(root_ext2, path);
  } else {
    ok = root_ramfs != NULL && ramfs_unlink(root_ramfs, path);
  }
  if (ok) { lookup_cache_invalidate_all(); }
  return ok;
}

bool vfs_link(const char *old_path, const char *new_path) {
  update_time_sources();
  bool ok = false;
  if (root_ext2 != NULL && !ramfs_route(old_path) && !ramfs_route(new_path)) {
    ok = ext2_link(root_ext2, old_path, new_path);
  } else if (root_ramfs != NULL && same_ramfs_route(old_path, new_path)) {
    ok = ramfs_link(root_ramfs, old_path, new_path);
  }
  if (ok) { lookup_cache_invalidate_all(); }
  return ok;
}

bool vfs_symlink(const char *target, const char *link_path) {
  update_time_sources();
  bool ok = root_ext2 != NULL && !ramfs_route(link_path) && ext2_symlink(root_ext2, target, link_path);
  if (ok) { lookup_cache_invalidate_all(); }
  return ok;
}

bool vfs_readlink(const char *path, char *out, size_t cap, size_t *len_out) {
  if (root_ext2 != NULL && !ramfs_route(path)) { return ext2_readlink(root_ext2, path, out, cap, len_out); }
  return false;
}

bool vfs_chmod(const char *path, uint32_t mode) {
  update_time_sources();
  bool ok = false;
  if (root_ext2 != NULL && !ramfs_route(path)) {
    ok = ext2_chmod(root_ext2, path, mode);
  } else {
    struct ramfs_node node;
    ok = root_ramfs != NULL && ramfs_route(path) && ramfs_lookup_node(root_ramfs, path, &node) &&
         ramfs_chmod_node(root_ramfs, node.index, (uint16_t)mode);
  }
  if (ok) { lookup_cache_invalidate_all(); }
  return ok;
}

bool vfs_chmod_node(const struct vfs_node *node, uint32_t mode) {
  update_time_sources();
  bool ok = false;
  if (node->backend == VFS_EXT2) {
    ok = ext2_chmod_node(root_ext2, &node->ext2, mode);
  } else if (node->backend == VFS_RAMFS) {
    ok = ramfs_chmod_node(node->ramfs.fs, node->ramfs.index, (uint16_t)mode);
  }
  if (ok) { lookup_cache_invalidate_all(); }
  return ok;
}

bool vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
  update_time_sources();
  bool ok = false;
  if (root_ext2 != NULL && !ramfs_route(path)) {
    ok = ext2_chown(root_ext2, path, uid, gid);
  } else {
    struct ramfs_node node;
    ok = root_ramfs != NULL && ramfs_route(path) && ramfs_lookup_node(root_ramfs, path, &node) &&
         ramfs_chown_node(root_ramfs, node.index, uid, gid);
  }
  if (ok) { lookup_cache_invalidate_all(); }
  return ok;
}

bool vfs_chown_node(const struct vfs_node *node, uint32_t uid, uint32_t gid) {
  update_time_sources();
  bool ok = false;
  if (node->backend == VFS_EXT2) {
    ok = ext2_chown_node(root_ext2, &node->ext2, uid, gid);
  } else if (node->backend == VFS_RAMFS) {
    ok = ramfs_chown_node(node->ramfs.fs, node->ramfs.index, uid, gid);
  }
  if (ok) { lookup_cache_invalidate_all(); }
  return ok;
}

bool vfs_rename(const char *old_path, const char *new_path) {
  update_time_sources();
  bool ok = false;
  if (root_ext2 != NULL && !ramfs_route(old_path) && !ramfs_route(new_path)) {
    ok = ext2_rename(root_ext2, old_path, new_path);
  } else if (root_ramfs != NULL && same_ramfs_route(old_path, new_path)) {
    ok = ramfs_rename(root_ramfs, old_path, new_path);
  }
  if (ok) { lookup_cache_invalidate_all(); }
  return ok;
}

uint64_t vfs_read(const struct vfs_node *node, uint64_t off, void *dst, uint64_t len) {
  if (node->backend == VFS_RAMFS) { return ramfs_read(node->ramfs.fs, node->ramfs.index, off, dst, len); }
  if (node->backend == VFS_EXT2) {
    uint32_t got = 0;
    if (len > UINT32_MAX) { len = UINT32_MAX; }
    return ext2_read_file(root_ext2, &node->ext2, off, dst, (uint32_t)len, &got) ? got : 0;
  }
  return 0;
}

int64_t vfs_write(const struct vfs_node *node, uint64_t off, const void *src, uint64_t len) {
  update_time_sources();
  vfs_page_cache_invalidate(node);
  int64_t written = -28;
  if (node->backend == VFS_EXT2) {
    struct ext2_node ext = node->ext2;
    written = ext2_write_file(root_ext2, &ext, off, src, len);
  } else if (node->backend == VFS_RAMFS) {
    written = ramfs_write(node->ramfs.fs, node->ramfs.index, off, src, len);
  }
  if (written >= 0) { lookup_cache_invalidate_all(); }
  return written;
}

bool vfs_refresh(const struct vfs_node *node, struct vfs_node *out) {
  if (node->backend == VFS_RAMFS) {
    struct ramfs_node fresh;
    if (!ramfs_refresh_node(node->ramfs.fs, node->ramfs.index, &fresh)) { return false; }
    from_ramfs(&fresh, out);
    return true;
  }
  if (node->backend == VFS_EXT2) {
    struct ext2_node fresh;
    if (!ext2_inode(root_ext2, node->ext2.ino, &fresh)) { return false; }
    *out = *node;
    from_ext2(&fresh, out);
    out->writable = true;
    return true;
  }
  if (node->backend == VFS_PROC) {
    *out = *node;
    return cell_proc_exists(node->proc_pid);
  }
  return false;
}

bool vfs_dirent(const struct vfs_node *dir, size_t index, struct vfs_dirent *out) {
  ++vfs_stat_counters.dirent_count;
  if (dir->backend == VFS_PROC) {
    if (dir->proc_pid < 0) {
      if (index != 0) { return false; }
      int pid = -dir->proc_pid;
      *out = (struct vfs_dirent){.ino = 130000u + (uint64_t)pid, .is_dir = true, .is_device = false};
      utoa_dec((uint64_t)pid, out->name, sizeof(out->name));
      return true;
    }
    static const char *names[] = {"stat", "status", "cmdline", "statm", "comm", "mounts", "cwd", "exe", "task"};
    if (!dir->is_dir || index >= sizeof(names) / sizeof(names[0])) { return false; }
    bool is_task = streq(names[index], "task");
    *out = (struct vfs_dirent){.ino = dir->ino + index + 1, .is_dir = is_task, .is_device = false};
    kmemcpy(out->name, names[index], kstrlen(names[index]) + 1);
    return true;
  }
  if (dir->backend == VFS_RAMFS) {
    struct ramfs_dirent ent;
    if (ramfs_dirent(dir->ramfs.fs, dir->ramfs.index, index, &ent)) {
      *out = (struct vfs_dirent){.ino = ent.ino, .is_dir = ent.is_dir, .is_device = ent.is_device, .type = ent.type};
      kmemcpy(out->name, ent.name, kstrlen(ent.name) + 1);
      return true;
    }
    if (dir->ramfs.mount == RAMFS_MOUNT_PROC && streq(dir->ramfs.name, "proc")) {
      size_t static_count = 0;
      while (ramfs_dirent(dir->ramfs.fs, dir->ramfs.index, static_count, &ent)) {
        ++static_count;
      }
      if (index >= static_count) {
        int pid = cell_proc_pid_at(index - static_count);
        if (pid != 0) {
          *out = (struct vfs_dirent){.ino = 100000u + (uint64_t)pid, .is_dir = true, .is_device = false};
          utoa_dec((uint64_t)pid, out->name, sizeof(out->name));
          return true;
        }
      }
    }
    return false;
  }
  if (dir->backend == VFS_EXT2) {
    struct ext2_dirent ent;
    if (!ext2_dirent(root_ext2, &dir->ext2, index, &ent)) { return false; }
    *out = (struct vfs_dirent){.ino = ent.ino, .is_dir = ent.type == 2, .is_device = false};
    kmemcpy(out->name, ent.name, kstrlen(ent.name) + 1);
    return true;
  }
  return false;
}

bool vfs_next_dirent(const struct vfs_node *dir, uint64_t *cursor, struct vfs_dirent *out) {
  ++vfs_stat_counters.next_dirent_count;
  if (cursor == NULL) { return false; }
  if (dir->backend == VFS_EXT2) {
    struct ext2_dirent ent;
    if (!ext2_next_dirent(root_ext2, &dir->ext2, cursor, &ent)) { return false; }
    *out = (struct vfs_dirent){.ino = ent.ino, .is_dir = ent.type == 2, .is_device = false};
    kmemcpy(out->name, ent.name, kstrlen(ent.name) + 1);
    return true;
  }
  size_t index = (size_t)*cursor;
  if (!vfs_dirent(dir, index, out)) { return false; }
  *cursor = index + 1;
  return true;
}

bool vfs_fs_info(struct vfs_fs_info *out) {
  if (out == NULL) { return false; }
  if (root_ext2 != NULL) {
    struct ext2_info info;
    if (!ext2_info(root_ext2, &info)) { return false; }
    *out = (struct vfs_fs_info){
      .block_size = info.block_size,
      .block_count = info.block_count,
      .free_blocks = info.free_blocks,
      .inode_count = info.inode_count,
      .free_inodes = info.free_inodes,
    };
    return true;
  }
  *out = (struct vfs_fs_info){
    .block_size = 1,
    .block_count = 0,
    .free_blocks = 0,
    .inode_count = 0,
    .free_inodes = 0,
  };
  return true;
}

static void copy_cstr(char *dst, size_t cap, const char *src) {
  if (cap == 0) { return; }
  size_t i = 0;
  for (; i + 1 < cap && src[i] != '\0'; ++i) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

static void set_mount_info(struct vfs_mount_info *out, const char *source, const char *target, const char *fstype,
                           uint64_t block_size, uint64_t block_count, uint64_t free_blocks) {
  copy_cstr(out->source, sizeof(out->source), source);
  copy_cstr(out->target, sizeof(out->target), target);
  copy_cstr(out->fstype, sizeof(out->fstype), fstype);
  out->block_size = block_size;
  out->block_count = block_count;
  out->free_blocks = free_blocks;
}

size_t vfs_mount_info(struct vfs_mount_info *out, size_t cap) {
  enum { MOUNT_COUNT = 7 };
  if (out == NULL || cap == 0) { return MOUNT_COUNT; }

  size_t n = 0;
  struct vfs_fs_info root = {0};
  if (vfs_fs_info(&root) && n < cap) {
    set_mount_info(&out[n++], "ext2-root", "/", "ext2", root.block_size, root.block_count, root.free_blocks);
  }

  uint64_t tmp_blocks = pmm_total_pages();
  uint64_t tmp_free = pmm_free_pages();
  if (n < cap) {
    set_mount_info(&out[n++], "tmpfs", "/tmp", "tmpfs", RAMFS_PAGE_SIZE, tmp_blocks, tmp_free);
  }
  if (n < cap) {
    set_mount_info(&out[n++], "runfs", "/run", "tmpfs", RAMFS_PAGE_SIZE, tmp_blocks, tmp_free);
  }
  if (n < cap) {
    uint64_t boot_blocks = BOOT_IMAGE_BYTES / 1024;
    set_mount_info(&out[n++], "bootfs", "/dev/fs/boot", "fat16", 1024, boot_blocks, 0);
  }
  if (n < cap) { set_mount_info(&out[n++], "ramfs", "/dev/fs/ram0", "ramfs", 1024, 0, 0); }
  if (n < cap) { set_mount_info(&out[n++], "proc", "/proc", "proc", 1024, 0, 0); }
  if (n < cap) { set_mount_info(&out[n++], "devfs", "/dev", "devfs", 1024, 0, 0); }
  return MOUNT_COUNT;
}
