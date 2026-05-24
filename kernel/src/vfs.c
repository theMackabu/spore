#include "vfs.h"

#include "mem.h"
#include "mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

static struct ramfs *root_ramfs;
static struct ext2_fs *root_ext2;
static uint64_t root_hhdm;

bool cell_proc_exists(int pid);
int cell_proc_pid_at(size_t index);

enum {
  EXEC_CACHE_ENTRIES = 1,
  EXEC_CACHE_MAX = 4 * 1024 * 1024,
  BOOT_IMAGE_BYTES = 16 * 1024 * 1024,
  VFS_DEV_EXT2_ROOT = 0x0801,
  VFS_DEV_DEVFS = 0x0005,
  VFS_DEV_PROC = 0x0006,
  VFS_DEV_RAM0 = 0x0010,
  VFS_DEV_TMPFS = 0x0011,
};

struct exec_cache_entry {
  bool valid;
  char path[128];
  uint8_t *data;
  uint64_t cap;
  uint64_t size;
};

static struct exec_cache_entry exec_cache[EXEC_CACHE_ENTRIES];
static size_t exec_cache_clock;

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
  return path_is_mount(path, "/dev") || path_is_mount(path, "/proc") || path_is_mount(path, "/tmp");
}

static bool same_ramfs_route(const char *a, const char *b) {
  return (path_is_mount(a, "/dev") && path_is_mount(b, "/dev")) ||
         (path_is_mount(a, "/proc") && path_is_mount(b, "/proc")) ||
         (path_is_mount(a, "/tmp") && path_is_mount(b, "/tmp"));
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

static void from_ramfs(const struct ramfs_node *node, struct vfs_node *out) {
  uint16_t perms = node->mode == 0 ? (node->is_dir ? 0777u : 0666u) : (uint16_t)(node->mode & 07777u);
  uint16_t mode = node->is_dir ? (uint16_t)(0040000u | perms) : (uint16_t)(0100000u | perms);
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
    .dev_id = ramfs_mount_dev_id(node->mount),
    .rdev = ramfs_device_rdev(node->device),
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
    .dev_id = VFS_DEV_EXT2_ROOT,
    .rdev = 0,
    .size = node->size,
    .ext2 = *node,
  };
}

void vfs_init(struct ramfs *ramfs, struct ext2_fs *ext2, uint64_t hhdm_offset) {
  root_ramfs = ramfs;
  root_ext2 = ext2;
  root_hhdm = hhdm_offset;
}

static bool streq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) { return false; }
  }
  return *a == '\0' && *b == '\0';
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
  } else if (streq(name, "cwd")) {
    *device = RAMFS_DEV_PROC_PID_CWD;
  } else if (streq(name, "exe")) {
    *device = RAMFS_DEV_PROC_PID_EXE;
  } else {
    return false;
  }
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
    *out = (struct vfs_node){
      .backend = VFS_PROC,
      .ino = 100000u + (uint64_t)pid,
      .is_dir = true,
      .writable = false,
      .device = RAMFS_DEV_NONE,
      .mode = 0040555u,
      .links_count = 1,
      .dev_id = VFS_DEV_PROC,
      .proc_pid = pid,
    };
    return true;
  }
  if (*p != '/') { return false; }
  ++p;
  enum ramfs_device device;
  if (!proc_kind_for_name(p, &device)) { return false; }
  *out = (struct vfs_node){
    .backend = VFS_PROC,
    .ino = 110000u + (uint64_t)pid * 8u + (uint64_t)device,
    .is_dir = false,
    .writable = false,
    .device = device,
    .mode = 0100444u,
    .links_count = 1,
    .dev_id = VFS_DEV_PROC,
    .proc_pid = pid,
  };
  return true;
}

static void copy_path(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  while (i + 1 < cap && src[i] != '\0') {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

static void invalidate_exec_cache(void) {
  for (size_t i = 0; i < EXEC_CACHE_ENTRIES; ++i) {
    exec_cache[i].valid = false;
  }
}

static struct exec_cache_entry *find_exec_cache(const char *path, uint64_t size) {
  for (size_t i = 0; i < EXEC_CACHE_ENTRIES; ++i) {
    if (exec_cache[i].valid && exec_cache[i].size == size && streq(exec_cache[i].path, path)) { return &exec_cache[i]; }
  }
  return NULL;
}

static bool allocate_exec_entry(struct exec_cache_entry *entry, uint64_t need) {
  if (entry->data != NULL && need <= entry->cap) { return true; }
  if (need > EXEC_CACHE_MAX) { return false; }
  uint64_t pages = EXEC_CACHE_MAX / PAGE_SIZE;
  uint64_t first = 0;
  for (uint64_t i = 0; i < pages; ++i) {
    uint64_t pa = pmm_alloc_zero_page();
    if (pa == 0) { return false; }
    if (i == 0) {
      first = pa;
    } else if (pa != first + i * PAGE_SIZE) {
      return false;
    }
  }
  entry->data = (uint8_t *)(uintptr_t)(root_hhdm + first);
  entry->cap = pages * PAGE_SIZE;
  return true;
}

static struct exec_cache_entry *next_exec_entry(uint64_t need) {
  struct exec_cache_entry *entry = &exec_cache[exec_cache_clock++ % EXEC_CACHE_ENTRIES];
  entry->valid = false;
  return allocate_exec_entry(entry, need) ? entry : NULL;
}

uint64_t vfs_exec_cache_pages(void) {
  uint64_t pages = 0;
  for (size_t i = 0; i < EXEC_CACHE_ENTRIES; ++i) {
    if (exec_cache[i].data != NULL) { pages += exec_cache[i].cap / PAGE_SIZE; }
  }
  return pages;
}

static bool lookup_ramfs(const char *path, struct vfs_node *out) {
  struct ramfs_node node;
  if (root_ramfs == NULL || !ramfs_lookup_node(root_ramfs, path, &node)) { return false; }
  from_ramfs(&node, out);
  return true;
}

bool vfs_lookup(const char *path, struct vfs_node *out) {
  if (ramfs_route(path)) {
    if (lookup_proc_dynamic(path, out)) { return true; }
    if (lookup_ramfs(path, out)) { return true; }
  }
  if (root_ext2 != NULL) {
    struct ext2_node node;
    if (ext2_lookup(root_ext2, path, &node)) {
      from_ext2(&node, out);
      return true;
    }
  }
  return lookup_ramfs(path, out);
}

bool vfs_lstat(const char *path, struct vfs_node *out) {
  if (ramfs_route(path)) {
    if (lookup_proc_dynamic(path, out)) { return true; }
    if (lookup_ramfs(path, out)) { return true; }
  }
  if (root_ext2 != NULL) {
    struct ext2_node node;
    if (ext2_lstat(root_ext2, path, &node)) {
      from_ext2(&node, out);
      return true;
    }
  }
  return lookup_ramfs(path, out);
}

bool vfs_lookup_exec(const char *path, const void **data, uint64_t *size) {
  if (root_ext2 != NULL) {
    struct ext2_node node;
    if (ext2_lookup(root_ext2, path, &node) && ext2_is_regular(&node)) {
      struct exec_cache_entry *entry = find_exec_cache(path, node.size);
      if (entry != NULL) {
        *data = entry->data;
        *size = entry->size;
        return true;
      }
      entry = next_exec_entry(node.size);
      if (entry == NULL) { return false; }
      uint32_t got = 0;
      if (!ext2_read_file(root_ext2, &node, 0, entry->data, node.size, &got) || got != node.size) { return false; }
      copy_path(entry->path, sizeof(entry->path), path);
      entry->size = node.size;
      entry->valid = true;
      *data = entry->data;
      *size = entry->size;
      return true;
    }
  }

  struct ramfs_file file;
  if (root_ramfs != NULL && ramfs_lookup(root_ramfs, path, &file)) {
    *data = file.data;
    *size = file.size;
    return true;
  }
  return false;
}

bool vfs_mkdir(const char *path) {
  if (root_ext2 != NULL && !ramfs_route(path)) {
    struct ext2_node node;
    bool ok = ext2_create(root_ext2, path, true, &node);
    if (ok) { invalidate_exec_cache(); }
    return ok;
  }
  return root_ramfs != NULL && ramfs_mkdir(root_ramfs, path);
}

bool vfs_create(const char *path, struct vfs_node *out) {
  if (root_ext2 != NULL && !ramfs_route(path)) {
    struct ext2_node node;
    if (!ext2_create(root_ext2, path, false, &node)) { return false; }
    from_ext2(&node, out);
    out->writable = true;
    invalidate_exec_cache();
    return true;
  }
  struct ramfs_node node;
  if (root_ramfs == NULL || !ramfs_create(root_ramfs, path, &node)) { return false; }
  from_ramfs(&node, out);
  return true;
}

bool vfs_truncate(const struct vfs_node *node, uint64_t size) {
  if (node->backend == VFS_EXT2) {
    struct ext2_node ext = node->ext2;
    bool ok = ext2_truncate(root_ext2, &ext, size);
    if (ok) { invalidate_exec_cache(); }
    return ok;
  }
  if (node->backend != VFS_RAMFS) { return false; }
  return ramfs_truncate(node->ramfs.fs, node->ramfs.index, size);
}

bool vfs_unlink(const char *path) {
  if (root_ext2 != NULL && !ramfs_route(path)) {
    bool ok = ext2_unlink(root_ext2, path);
    if (ok) { invalidate_exec_cache(); }
    return ok;
  }
  return root_ramfs != NULL && ramfs_unlink(root_ramfs, path);
}

bool vfs_link(const char *old_path, const char *new_path) {
  if (root_ext2 != NULL && !ramfs_route(old_path) && !ramfs_route(new_path)) {
    bool ok = ext2_link(root_ext2, old_path, new_path);
    if (ok) { invalidate_exec_cache(); }
    return ok;
  }
  if (root_ramfs != NULL && same_ramfs_route(old_path, new_path)) { return ramfs_link(root_ramfs, old_path, new_path); }
  return false;
}

bool vfs_symlink(const char *target, const char *link_path) {
  if (root_ext2 != NULL && !ramfs_route(link_path)) {
    bool ok = ext2_symlink(root_ext2, target, link_path);
    if (ok) { invalidate_exec_cache(); }
    return ok;
  }
  return false;
}

bool vfs_readlink(const char *path, char *out, size_t cap, size_t *len_out) {
  if (root_ext2 != NULL && !ramfs_route(path)) { return ext2_readlink(root_ext2, path, out, cap, len_out); }
  return false;
}

bool vfs_chmod(const char *path, uint32_t mode) {
  if (root_ext2 != NULL && !ramfs_route(path)) { return ext2_chmod(root_ext2, path, mode); }
  struct ramfs_node node;
  return root_ramfs != NULL && ramfs_route(path) && ramfs_lookup_node(root_ramfs, path, &node) &&
         ramfs_chmod_node(root_ramfs, node.index, (uint16_t)mode);
}

bool vfs_chmod_node(const struct vfs_node *node, uint32_t mode) {
  if (node->backend == VFS_EXT2) { return ext2_chmod_node(root_ext2, &node->ext2, mode); }
  return node->backend == VFS_RAMFS && ramfs_chmod_node(node->ramfs.fs, node->ramfs.index, (uint16_t)mode);
}

bool vfs_rename(const char *old_path, const char *new_path) {
  if (root_ext2 != NULL && !ramfs_route(old_path) && !ramfs_route(new_path)) {
    bool ok = ext2_rename(root_ext2, old_path, new_path);
    if (ok) { invalidate_exec_cache(); }
    return ok;
  }
  return root_ramfs != NULL && same_ramfs_route(old_path, new_path) && ramfs_rename(root_ramfs, old_path, new_path);
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
  if (node->backend == VFS_EXT2) {
    struct ext2_node ext = node->ext2;
    int64_t wrote = ext2_write_file(root_ext2, &ext, off, src, len);
    if (wrote >= 0) { invalidate_exec_cache(); }
    return wrote;
  }
  if (node->backend != VFS_RAMFS) { return -28; }
  return ramfs_write(node->ramfs.fs, node->ramfs.index, off, src, len);
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
  if (dir->backend == VFS_PROC) {
    static const char *names[] = {"stat", "status", "cmdline", "cwd", "exe"};
    if (!dir->is_dir || index >= sizeof(names) / sizeof(names[0])) { return false; }
    *out = (struct vfs_dirent){.ino = dir->ino + index + 1, .is_dir = false, .is_device = false};
    kmemcpy(out->name, names[index], kstrlen(names[index]) + 1);
    return true;
  }
  if (dir->backend == VFS_RAMFS) {
    struct ramfs_dirent ent;
    if (ramfs_dirent(dir->ramfs.fs, dir->ramfs.index, index, &ent)) {
      *out = (struct vfs_dirent){.ino = ent.ino, .is_dir = ent.is_dir, .is_device = ent.is_device};
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
  enum { MOUNT_COUNT = 6 };
  if (out == NULL || cap == 0) { return MOUNT_COUNT; }

  size_t n = 0;
  struct vfs_fs_info root = {0};
  if (vfs_fs_info(&root) && n < cap) {
    set_mount_info(&out[n++], "ext2-root", "/", "ext2", root.block_size, root.block_count, root.free_blocks);
  }

  uint64_t tmp_blocks = RAMFS_FILE_CAP / RAMFS_PAGE_SIZE;
  uint64_t tmp_used = ramfs_backing_used_pages();
  if (n < cap) {
    set_mount_info(&out[n++], "tmpfs", "/tmp", "tmpfs", RAMFS_PAGE_SIZE, tmp_blocks,
                   tmp_used < tmp_blocks ? tmp_blocks - tmp_used : 0);
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
