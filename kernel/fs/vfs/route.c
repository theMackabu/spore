#include "internal.h"
#include "init/app.h"

#include "fs/procfs.h"

#include "proc/accounting.h"
#include "proc/domain.h"
#include "proc/limits.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

constexpr int BOOT_IMAGE_BYTES = 16 * 1024 * 1024;

static bool path_is_mount(const char *path, const char *mount) {
  if (!starts_with(path, mount)) { return false; }
  char next = path[kstrlen(mount)];
  return next == '\0' || next == '/';
}

static bool data_path(const char *path) {
  return vfs_root_ext2 && path_is_mount(path, "/data");
}

const char *vfs_disk_path(const char *path) {
  struct ramfs_route route = mountns_lookup(path);
  if (route.boot) { return route.boot->device == VFS_DEV_EXT2_ROOT ? route.path : NULL; }
  if (!app_mode) { return path; }
  if (!data_path(path)) { return NULL; }
  return path[5] ? path + 5 : "/";
}

bool vfs_readonly_path(const char *path) {
  struct ramfs_route route = mountns_lookup(path);
  if (route.access) { return vfs_mount_flag(route.access, MOUNT_RDONLY); }
  if (route.fs) { return false; }
  return app_mode && !data_path(path) && !path_is_mount(path, "/tmp") && !path_is_mount(path, "/run") &&
         !path_is_mount(path, "/dev/shm");
}

bool vfs_readonly_node(const struct vfs_node *node) {
  if (node->access && node->access->proc_mount && node->backend == VFS_RAMFS) { return true; }
  if (node->backend == VFS_NSFS || vfs_mount_flag(node->access, MOUNT_RDONLY)) { return true; }
  return app_mode && node->backend == VFS_RAMFS && node->ramfs.mount == RAMFS_MOUNT_RAM0;
}

bool vfs_path_uses_ramfs(const char *path) {
  if (app_mode) { return !data_path(path); }
  return path_is_mount(path, "/dev") || path_is_mount(path, "/proc") || path_is_mount(path, "/sys") ||
         path_is_mount(path, "/tmp") || path_is_mount(path, "/run");
}

bool vfs_paths_share_ramfs(const char *a, const char *b) {
  return (path_is_mount(a, "/dev") && path_is_mount(b, "/dev")) ||
         (path_is_mount(a, "/proc") && path_is_mount(b, "/proc")) ||
         (path_is_mount(a, "/sys") && path_is_mount(b, "/sys")) ||
         (path_is_mount(a, "/tmp") && path_is_mount(b, "/tmp")) ||
         (path_is_mount(a, "/run") && path_is_mount(b, "/run"));
}

static uint64_t make_rdev(uint64_t major, uint64_t minor) {
  return (major << 8) | minor;
}

static uint64_t ramfs_mount_dev_id(const struct ramfs_node *node) {
  if (node->fs && node->fs->device) { return node->fs->device; }
  if (node->device == RAMFS_DEV_FS_BOOT) { return VFS_DEV_BOOT; }
  if (node->device == RAMFS_DEV_FS_RAM0) { return VFS_DEV_RAM0; }
  switch (node->mount) {
  case RAMFS_MOUNT_DEV:
    return VFS_DEV_DEVFS;
  case RAMFS_MOUNT_PROC:
    return VFS_DEV_PROC;
  case RAMFS_MOUNT_SYS:
    return VFS_DEV_SYSFS;
  case RAMFS_MOUNT_TMP:
    return VFS_DEV_TMPFS;
  case RAMFS_MOUNT_RUN:
    return VFS_DEV_RUNFS;
  case RAMFS_MOUNT_RAM0:
    return VFS_DEV_RAM0;
  }
  return VFS_DEV_RAM0;
}

static uint64_t ramfs_device_rdev(const struct ramfs_node *node) {
  switch (node->device) {
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
  case RAMFS_DEV_PTMX:
    return make_rdev(5, 2);
  case RAMFS_DEV_PTS: {
    unsigned minor = 0;
    for (const char *p = node->name; p && *p >= '0' && *p <= '9'; ++p) { minor = minor * 10 + (*p - '0'); }
    return make_rdev(136, minor);
  }
  case RAMFS_DEV_BLK_ROOT:
    return make_rdev(254, 0);
  case RAMFS_DEV_BLK_BOOT:
    return make_rdev(254, 1);
  default:
    return 0;
  }
}

void vfs_update_time_sources(void) {
  uint64_t now = cell_realtime_seconds();
  ramfs_set_now(now);
  ext2_set_now((uint32_t)now);
}

void vfs_from_ramfs(const struct ramfs_node *node, struct vfs_node *out) {
  *out = (struct vfs_node){
    .backend = VFS_RAMFS,
    .ino = node->ino,
    .is_dir = node->is_dir,
    .device = node->device,
    .mode = node->mode,
    .links_count = node->links_count,
    .uid = node->uid,
    .gid = node->gid,
    .dev_id = ramfs_mount_dev_id(node),
    .rdev = ramfs_device_rdev(node),
    .atime = node->atime,
    .ctime = node->ctime,
    .mtime = node->mtime,
    .size = node->size,
    .ramfs = *node,
  };
}

void vfs_from_ext2(const struct ext2_node *node, struct vfs_node *out) {
  *out = (struct vfs_node){
    .backend = VFS_EXT2,
    .ino = node->ino,
    .is_dir = ext2_is_dir(node),
    .device = RAMFS_DEV_NONE,
    .mode = node->mode,
    .links_count = node->links_count,
    .uid = node->uid,
    .gid = node->gid,
    .dev_id = VFS_DEV_EXT2_ROOT,
    .rdev = 0,
    .atime = (int32_t)node->atime,
    .ctime = (int32_t)node->ctime,
    .mtime = (int32_t)node->mtime,
    .size = node->size,
    .ext2 = *node,
  };
}

static bool lookup_ramfs(struct ramfs *fs, int root, const char *path, struct vfs_node *out) {
  struct ramfs_node node;
  if (!fs || !ramfs_lookup_node(fs, root, path, &node)) { return false; }
  vfs_from_ramfs(&node, out);
  return true;
}

bool vfs_lookup_component(const char *path, bool nofollow, struct vfs_node *out) {
  struct ramfs_route mounted = mountns_lookup(path);
  if (mounted.boot && mounted.boot->device == VFS_DEV_PROC && mounted.path[1]) {
    char proc[CELL_PATH_MAX], base[CELL_PATH_MAX];
    const char *dir = ramfs_inode_path(mounted.fs, mounted.root, base, sizeof(base));
    size_t len = kstrlen(mounted.path);
    if (!dir || kstrlen(dir) + len >= sizeof(proc)) { return false; }
    kmemcpy(proc, dir, kstrlen(dir));
    kmemcpy(proc + kstrlen(dir), mounted.path, len + 1);
    if (vfs_proc_lookup(mounted.access, proc, !nofollow, out)) { out->mount_id = mounted.mount_id; out->access = mounted.access; return true; }
  }
  if (mounted.fs) {
    if (!lookup_ramfs(mounted.fs, mounted.root, mounted.path, out)) { return false; }
    out->mount_id = mounted.mount_id; out->access = mounted.access;
    return true;
  }
  if (mounted.boot) {
    struct ext2_node node;
    if (!vfs_root_ext2 || mounted.boot->device != VFS_DEV_EXT2_ROOT ||
        !(nofollow ? ext2_lstat_from(vfs_root_ext2, mounted.root ? mounted.root : EXT2_ROOT_INO, mounted.path, &node) : ext2_lookup_from(vfs_root_ext2, mounted.root ? mounted.root : EXT2_ROOT_INO, mounted.path, &node))) { return false; }
    vfs_from_ext2(&node, out);
    out->mount_id = mounted.mount_id; out->access = mounted.access;
    return true;
  }
  if (vfs_lookup_cache_get(path, nofollow, out)) { return true; }
  if (vfs_path_uses_ramfs(path)) {
    if (vfs_proc_lookup(NULL, path, !nofollow, out)) { return true; }
    if (lookup_ramfs(vfs_root_ramfs, 0, path, out)) {
      vfs_lookup_cache_put(path, nofollow, out);
      return true;
    }
  }
  const char *disk_path = vfs_root_ext2 ? vfs_disk_path(path) : NULL;
  struct ext2_node node;
  if (disk_path && (nofollow ? ext2_lstat(vfs_root_ext2, disk_path, &node) : ext2_lookup(vfs_root_ext2, disk_path, &node))) {
    vfs_from_ext2(&node, out);
  } else if (!lookup_ramfs(vfs_root_ramfs, 0, path, out)) { return false; }
  vfs_lookup_cache_put(path, nofollow, out);
  return true;
}

static bool lookup_path(const char *path, bool nofollow, struct vfs_node *out) {
  if (!path || *path != '/') { return false; }
  if (!mountns_current_routes() && !ramfs_has_symlinks() && !vfs_path_has_proc(path, kstrlen(path))) {
    return vfs_lookup_component(path, nofollow, out);
  }
  char canonical[CELL_PATH_MAX];
  const char *root = cell_current_chroot();
  size_t prefix = root[1] ? kstrlen(root) : 0;
  if (prefix && (!starts_with(path, root) || (path[prefix] && path[prefix] != '/'))) { return false; }
  return !vfs_walk(root, "/", 1, path + prefix, kstrlen(path + prefix), !nofollow, canonical, sizeof(canonical)) &&
         vfs_lookup_component(canonical, nofollow, out);
}

bool vfs_lookup(const char *path, struct vfs_node *out) {
  ++vfs_stat_counters.lookup_count;
  return lookup_path(path, false, out);
}

bool vfs_lstat(const char *path, struct vfs_node *out) {
  ++vfs_stat_counters.lstat_count;
  return lookup_path(path, true, out);
}

bool vfs_locate_entry(const char *path, const struct vfs_node *followed, struct vfs_entry_name *location) {
  if (!path) { return false; }
  if (followed && followed->backend == VFS_EXT2 && (followed->mode & 0170000u) != 0120000u) {
    struct ramfs_route route = mountns_lookup(path);
    if (!ext2_locate_entry_from(vfs_root_ext2, route.root ? route.root : EXT2_ROOT_INO, vfs_disk_path(path), followed->ext2.ino, location)) {
      location->parent_ino = 0;
      return false;
    }
    location->parent_dev = followed->dev_id;
    return true;
  }
  size_t end = kstrlen(path);
  while (end && path[end - 1] == '/') { --end; }
  size_t start = end;
  while (start && path[start - 1] != '/') { --start; }
  if (!start || end - start >= sizeof(location->name)) { return false; }
  char parent[CELL_PATH_MAX];
  size_t prefix = start == 1 ? 1 : start - 1;
  if (prefix >= sizeof(parent)) { return false; }
  kmemcpy(parent, path, prefix);
  parent[prefix] = 0;
  struct vfs_node node;
  if (!vfs_lookup(parent, &node)) { return false; }
  location->parent_dev = node.dev_id;
  location->parent_ino = node.ino;
  kmemcpy(location->name, path + start, end - start);
  location->name[end - start] = 0;
  return true;
}

bool vfs_readlink(const struct vfs_node *node, char *out, size_t cap, size_t *len_out) {
  if (node->backend == VFS_RAMFS && (node->mode & 0170000) == 0120000) {
    size_t n = node->size < cap ? node->size : cap;
    if (ramfs_read(node->ramfs.fs, node->ramfs.index, 0, out, n) != n) { return false; }
    if (n < cap) { out[n] = 0; }
    if (len_out) { *len_out = n; }
    return true;
  }
  if (node->backend == VFS_PROC) { return vfs_proc_readlink(node, out, cap, len_out); }
  if (node->backend == VFS_EXT2) { return ext2_readlink_node(vfs_root_ext2, &node->ext2, out, cap, len_out); }
  return false;
}

int vfs_next_dirent(const struct vfs_node *dir, uint64_t *cursor, struct vfs_dirent *out) {
  ++vfs_stat_counters.next_dirent_count;
  if (cursor == NULL) { return 0; }
  if (dir->backend == VFS_EXT2) { return ext2_next_dirent(vfs_root_ext2, &dir->ext2, cursor, out); }
  if (dir->backend == VFS_RAMFS && ramfs_next_dirent(dir->ramfs.fs, dir->ramfs.index, cursor, out)) { return 1; }
  return vfs_proc_next_dirent(dir, cursor, out);
}

bool vfs_fs_info(struct vfs_fs_info *out) {
  if (out == NULL) { return false; }
  if (vfs_root_ext2 != NULL) {
    struct ext2_info info;
    if (!ext2_info(vfs_root_ext2, &info)) { return false; }
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

static const struct vfs_mount desktop_mounts[] = {
  {VFS_DEV_EXT2_ROOT, VFS_DEV_EXT2_ROOT, 0xef53, "ext2-root", "/", "ext2", "rw", 0},
  {VFS_DEV_TMPFS, VFS_DEV_EXT2_ROOT, 0x01021994, "tmpfs", "/tmp", "tmpfs", "rw", 0},
  {VFS_DEV_RUNFS, VFS_DEV_EXT2_ROOT, 0x01021994, "runfs", "/run", "tmpfs", "rw", 0},
  {VFS_DEV_BOOT, VFS_DEV_DEVFS, 0x4d44, "bootfs", "/dev/fs/boot", "fat16", "ro", 0},
  {VFS_DEV_RAM0, VFS_DEV_DEVFS, 0x858458f6, "ramfs", "/dev/fs/ram0", "ramfs", "ro", 0},
  {VFS_DEV_PROC, VFS_DEV_EXT2_ROOT, 0x9fa0, "proc", "/proc", "proc", "ro", 0},
  {VFS_DEV_SYSFS, VFS_DEV_EXT2_ROOT, 0x62656572, "sysfs", "/sys", "sysfs", "ro", 0},
  {VFS_DEV_DEVFS, VFS_DEV_EXT2_ROOT, 0x1373, "devfs", "/dev", "devfs", "rw", 0},
};

static const struct vfs_mount app_mounts[] = {
  {VFS_DEV_RAM0, VFS_DEV_RAM0, 0x858458f6, "app", "/", "ramfs", "ro", 0},
  {VFS_DEV_TMPFS, VFS_DEV_RAM0, 0x01021994, "tmpfs", "/tmp", "tmpfs", "rw", 0},
  {VFS_DEV_RUNFS, VFS_DEV_RAM0, 0x01021994, "runfs", "/run", "tmpfs", "rw", 0},
  {VFS_DEV_PROC, VFS_DEV_RAM0, 0x9fa0, "proc", "/proc", "proc", "ro", 0},
  {VFS_DEV_SYSFS, VFS_DEV_RAM0, 0x62656572, "sysfs", "/sys", "sysfs", "ro", 0},
  {VFS_DEV_DEVFS, VFS_DEV_RAM0, 0x1373, "devfs", "/dev", "devfs", "rw", 0},
  {VFS_DEV_EXT2_ROOT, VFS_DEV_RAM0, 0xef53, "data", "/data", "ext2", "rw", 0},
};

static size_t mount_count(void) {
  return app_mode ? sizeof(app_mounts)/sizeof(*app_mounts) - (vfs_root_ext2 == NULL)
                  : sizeof(desktop_mounts)/sizeof(*desktop_mounts);
}

const struct vfs_mount *vfs_boot_mount_at(size_t index) {
  return index < mount_count() ? &(app_mode ? app_mounts : desktop_mounts)[index] : NULL;
}

const struct vfs_mount *vfs_domain_mount_at(const struct domain *domain, size_t index) {
  if (mountns_explicit(domain)) { return mountns_at(domain, index); }
  size_t base = mount_count();
  return index < base ? &(app_mode ? app_mounts : desktop_mounts)[index] : mountns_at(domain, index - base);
}

const struct vfs_mount *vfs_mount_at(size_t index) {
  return vfs_domain_mount_at(cell_current_domain_internal(), index);
}

static bool mount_stats(const struct vfs_mount *mount, struct vfs_fs_info *out) {
  *out = (struct vfs_fs_info){.block_size = 1024};
  switch (mount->magic == 0x01021994 ? VFS_DEV_TMPFS : mount->device) {
  case VFS_DEV_RAM0:
    if (app_mode) { out->block_count = (app_payload_size() + 1023) / 1024; }
    break;
  case VFS_DEV_EXT2_ROOT:
    if (!vfs_fs_info(out)) { return false; }
    break;
  case VFS_DEV_TMPFS:
  case VFS_DEV_RUNFS:
    out->block_size = RAMFS_PAGE_SIZE;
    out->block_count = pmm_total_pages();
    out->free_blocks = pmm_free_pages();
    break;
  case VFS_DEV_BOOT:
    out->block_count = BOOT_IMAGE_BYTES / out->block_size;
    break;
  }
  out->magic = mount->magic;
  return true;
}

bool vfs_device_fs_info(uint64_t dev_id, struct vfs_fs_info *out) {
  if (dev_id == VFS_DEV_NSFS) {
    *out = (struct vfs_fs_info){.magic = 0x6e736673, .block_size = PAGE_SIZE};
    return true;
  }
  if (dev_id >= 0x10000) { dev_id = VFS_DEV_TMPFS; }
  for (size_t i = 0; i < mount_count(); ++i) {
    if (vfs_boot_mount_at(i)->device == dev_id) { return mount_stats(vfs_boot_mount_at(i), out); }
  }
  return false;
}

bool vfs_mount_detail(size_t index, struct vfs_mount_info *out) {
  const struct vfs_mount *mount = vfs_mount_at(index);
  if (!mount) { return false; }
  struct vfs_fs_info stats;
  if (!mount_stats(mount, &stats)) { return false; }
  const char *fstype = mount->fstype;
  if (mount->device == VFS_DEV_EXT2_ROOT && vfs_root_ext2 && vfs_root_ext2->ext2plus) { fstype = "ext2+"; }
  *out = (struct vfs_mount_info){.block_size = stats.block_size, .block_count = stats.block_count,
                                 .free_blocks = stats.free_blocks};
  copy_cstr(out->source, sizeof(out->source), mount->source);
  copy_cstr(out->target, sizeof(out->target), mount->target);
  copy_cstr(out->fstype, sizeof(out->fstype), fstype);
  return true;
}
