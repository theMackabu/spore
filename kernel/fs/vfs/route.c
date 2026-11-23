#include "internal.h"

#include "fs/procfs.h"

#include "proc/accounting.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

enum { BOOT_IMAGE_BYTES = 16 * 1024 * 1024 };

static bool path_is_mount(const char *path, const char *mount) {
  if (!starts_with(path, mount)) { return false; }
  char next = path[kstrlen(mount)];
  return next == '\0' || next == '/';
}

bool vfs_path_uses_ramfs(const char *path) {
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

static bool ramfs_device_is_block(enum ramfs_device device) {
  return device == RAMFS_DEV_BLK_ROOT || device == RAMFS_DEV_BLK_BOOT;
}

static uint64_t make_rdev(uint64_t major, uint64_t minor) {
  return (major << 8) | minor;
}

static uint64_t ramfs_mount_dev_id(const struct ramfs_node *node) {
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
  case RAMFS_DEV_PTMX:
    return make_rdev(5, 2);
  case RAMFS_DEV_PTS:
    return make_rdev(136, 0);
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
  uint16_t perms = node->mode & 07777u;
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
    .links_count = node->links_count,
    .uid = node->uid,
    .gid = node->gid,
    .seals = node->seals,
    .shared_write_mappings = node->shared_write_mappings,
    .sealable = node->sealable,
    .dev_id = ramfs_mount_dev_id(node),
    .rdev = ramfs_device_rdev(node->device),
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

static bool vfs_lookup_ramfs(const char *path, struct vfs_node *out) {
  struct ramfs_node node;
  if (vfs_root_ramfs == NULL || !ramfs_lookup_node(vfs_root_ramfs, path, &node)) { return false; }
  vfs_from_ramfs(&node, out);
  return true;
}

bool vfs_lookup(const char *path, struct vfs_node *out) {
  ++vfs_stat_counters.lookup_count;
  if (vfs_lookup_cache_get(path, false, out)) { return true; }
  if (vfs_path_uses_ramfs(path)) {
    if (vfs_proc_lookup(path, out)) { return true; }
    if (vfs_lookup_ramfs(path, out)) {
      vfs_lookup_cache_put(path, false, out);
      return true;
    }
  }
  if (vfs_root_ext2 != NULL) {
    struct ext2_node node;
    if (ext2_lookup(vfs_root_ext2, path, &node)) {
      vfs_from_ext2(&node, out);
      vfs_lookup_cache_put(path, false, out);
      return true;
    }
  }
  if (!vfs_lookup_ramfs(path, out)) { return false; }
  vfs_lookup_cache_put(path, false, out);
  return true;
}

bool vfs_lstat(const char *path, struct vfs_node *out) {
  ++vfs_stat_counters.lstat_count;
  if (vfs_lookup_cache_get(path, true, out)) { return true; }
  if (vfs_path_uses_ramfs(path)) {
    if (vfs_proc_lookup(path, out)) { return true; }
    if (vfs_lookup_ramfs(path, out)) {
      vfs_lookup_cache_put(path, true, out);
      return true;
    }
  }
  if (vfs_root_ext2 != NULL) {
    struct ext2_node node;
    if (ext2_lstat(vfs_root_ext2, path, &node)) {
      vfs_from_ext2(&node, out);
      vfs_lookup_cache_put(path, true, out);
      return true;
    }
  }
  if (!vfs_lookup_ramfs(path, out)) { return false; }
  vfs_lookup_cache_put(path, true, out);
  return true;
}

bool vfs_readlink(const char *path, char *out, size_t cap, size_t *len_out) {
  if (vfs_path_uses_ramfs(path)) { return vfs_proc_readlink(path, out, cap, len_out); }
  if (vfs_root_ext2 != NULL) {
    return ext2_readlink(vfs_root_ext2, path, out, cap, len_out);
  }
  return false;
}

bool vfs_dirent(const struct vfs_node *dir, size_t index, struct vfs_dirent *out) {
  ++vfs_stat_counters.dirent_count;
  if (dir->backend == VFS_PROC) { return vfs_proc_dirent(dir, index, out); }
  if (dir->backend == VFS_RAMFS) {
    struct ramfs_dirent ent;
    if (ramfs_dirent(dir->ramfs.fs, dir->ramfs.index, index, &ent)) {
      *out = (struct vfs_dirent){.ino = ent.ino, .is_dir = ent.is_dir, .is_device = ent.is_device, .type = ent.type};
      kmemcpy(out->name, ent.name, kstrlen(ent.name) + 1);
      return true;
    }
    return vfs_proc_dirent(dir, index, out);
  }
  if (dir->backend == VFS_EXT2) {
    struct ext2_dirent ent;
    if (!ext2_dirent(vfs_root_ext2, &dir->ext2, index, &ent)) { return false; }
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
    if (!ext2_next_dirent(vfs_root_ext2, &dir->ext2, cursor, &ent)) { return false; }
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

static const struct vfs_mount mounts[] = {
  {VFS_DEV_EXT2_ROOT, VFS_DEV_EXT2_ROOT, 0xef53, "ext2-root", "/", "ext2", "rw"},
  {VFS_DEV_TMPFS, VFS_DEV_EXT2_ROOT, 0x01021994, "tmpfs", "/tmp", "tmpfs", "rw"},
  {VFS_DEV_RUNFS, VFS_DEV_EXT2_ROOT, 0x01021994, "runfs", "/run", "tmpfs", "rw"},
  {VFS_DEV_BOOT, VFS_DEV_DEVFS, 0x4d44, "bootfs", "/dev/fs/boot", "fat16", "ro"},
  {VFS_DEV_RAM0, VFS_DEV_DEVFS, 0x858458f6, "ramfs", "/dev/fs/ram0", "ramfs", "ro"},
  {VFS_DEV_PROC, VFS_DEV_EXT2_ROOT, 0x9fa0, "proc", "/proc", "proc", "ro"},
  {VFS_DEV_SYSFS, VFS_DEV_EXT2_ROOT, 0x62656572, "sysfs", "/sys", "sysfs", "ro"},
  {VFS_DEV_DEVFS, VFS_DEV_EXT2_ROOT, 0x1373, "devfs", "/dev", "devfs", "rw"},
};

const struct vfs_mount *vfs_mount_at(size_t index) {
  return index < sizeof(mounts) / sizeof(mounts[0]) ? &mounts[index] : NULL;
}

static bool mount_stats(size_t index, struct vfs_fs_info *out) {
  *out = (struct vfs_fs_info){.block_size = 1024};
  switch (mounts[index].device) {
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
  out->magic = mounts[index].magic;
  return true;
}

bool vfs_device_fs_info(uint64_t dev_id, struct vfs_fs_info *out) {
  for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); ++i) {
    if (mounts[i].device == dev_id) { return mount_stats(i, out); }
  }
  return false;
}

size_t vfs_mount_info(struct vfs_mount_info *out, size_t cap) {
  const size_t count = sizeof(mounts) / sizeof(mounts[0]);
  if (out == NULL) { return count; }
  for (size_t i = 0; i < cap && i < count; ++i) {
    struct vfs_fs_info stats;
    mount_stats(i, &stats);
    const char *fstype = mounts[i].fstype;
    if (mounts[i].device == VFS_DEV_EXT2_ROOT && vfs_root_ext2 != NULL && vfs_root_ext2->ext2plus) { fstype = "ext2+"; }
    copy_cstr(out[i].source, sizeof(out[i].source), mounts[i].source);
    copy_cstr(out[i].target, sizeof(out[i].target), mounts[i].target);
    copy_cstr(out[i].fstype, sizeof(out[i].fstype), fstype);
    out[i].block_size = stats.block_size;
    out[i].block_count = stats.block_count;
    out[i].free_blocks = stats.free_blocks;
  }
  return count;
}
