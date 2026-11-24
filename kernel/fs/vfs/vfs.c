#include "internal.h"

#include "lib/mem.h"
#include "fs/procfs.h"
#include "fs/notify.h"

#include <stddef.h>
#include <stdint.h>

struct ramfs *vfs_root_ramfs;
struct ext2_fs *vfs_root_ext2;
struct vfs_stats vfs_stat_counters;

static void ext2_inode_freed(uint32_t ino) {
  struct ext2_node ext = {.ino = ino};
  struct vfs_node node;
  vfs_from_ext2(&ext, &node);
  vfs_page_cache_forget(&node);
  fsnotify_reclaim(node.dev_id, ino);
}

static void ramfs_inode_freed(enum ramfs_mount mount, uint64_t ino) {
  struct ramfs_node ram = {.mount = mount, .ino = ino};
  struct vfs_node node;
  vfs_from_ramfs(&ram, &node);
  vfs_page_cache_forget(&node);
  fsnotify_reclaim(node.dev_id, ino);
}

void vfs_init(struct ramfs *ramfs, struct ext2_fs *ext2) {
  vfs_root_ramfs = ramfs;
  vfs_root_ext2 = ext2;
  if (ramfs) { ramfs_set_reclaim_callback(ramfs, ramfs_inode_freed); }
  if (ext2) { ext2->inode_freed = ext2_inode_freed; }
  vfs_page_cache_configure();
  vfs_update_time_sources();
}

struct vfs_stats vfs_get_stats(void) {
  struct vfs_stats stats = vfs_stat_counters;
  vfs_page_cache_stats(&stats);
  return stats;
}

bool vfs_sync_storage(void) {
  return vfs_root_ext2 == NULL || ext2_flush(vfs_root_ext2);
}

bool vfs_sync(void) {
  bool ok = vfs_sync_mapped_pages();
  return vfs_sync_storage() && ok;
}

uint64_t vfs_read_uncached(const struct vfs_node *node, uint64_t off, void *dst, uint64_t len) {
  if (node->backend == VFS_RAMFS) { return ramfs_read(node->ramfs.fs, node->ramfs.index, off, dst, len); }
  if (node->backend == VFS_EXT2) {
    uint32_t read = 0;
    if (len > UINT32_MAX) { len = UINT32_MAX; }
    return ext2_read_file(vfs_root_ext2, &node->ext2, off, dst, (uint32_t)len, &read) ? read : 0;
  }
  return 0;
}

bool vfs_mkdir(const char *path, uint32_t mode) {
  vfs_update_time_sources();
  bool ok = false;
  if (vfs_root_ext2 != NULL && !vfs_path_uses_ramfs(path)) {
    struct ext2_node node;
    ok = ext2_create(vfs_root_ext2, path, true, (uint16_t)mode, &node);
  } else {
    ok = vfs_root_ramfs != NULL && ramfs_mkdir(vfs_root_ramfs, path, (uint16_t)mode);
  }
  if (ok) { vfs_lookup_cache_clear(); fsnotify_path_event(path, FS_CREATE); }
  return ok;
}

bool vfs_create(const char *path, uint16_t mode, struct vfs_node *out) {
  vfs_update_time_sources();
  if (vfs_root_ext2 != NULL && !vfs_path_uses_ramfs(path)) {
    struct ext2_node node;
    if (!ext2_create(vfs_root_ext2, path, false, mode, &node)) { return false; }
    vfs_from_ext2(&node, out);
    vfs_lookup_cache_clear(); fsnotify_path_event(path, FS_CREATE);
    return true;
  }
  struct ramfs_node node;
  if (vfs_root_ramfs == NULL || !ramfs_create(vfs_root_ramfs, path, mode, &node)) { return false; }
  vfs_from_ramfs(&node, out);
  vfs_lookup_cache_clear(); fsnotify_path_event(path, FS_CREATE);
  return true;
}

int vfs_tmpfile(const struct vfs_node *dir, uint16_t mode, uint32_t uid, uint32_t gid, bool linkable,
                struct vfs_node *out) {
  vfs_update_time_sources();
  if (dir->backend == VFS_EXT2) {
    struct ext2_node node;
    if (!vfs_root_ext2->write) { return -30; }
    if (!ext2_tmpfile(vfs_root_ext2, mode, uid, gid, linkable, &node)) { return -28; }
    vfs_from_ext2(&node, out);
    return 0;
  }
  if (dir->backend == VFS_RAMFS && (dir->ramfs.mount == RAMFS_MOUNT_TMP || dir->ramfs.mount == RAMFS_MOUNT_RUN ||
                                    dir->ramfs.mount == RAMFS_MOUNT_RAM0 || dir->ramfs.mount == RAMFS_MOUNT_DEV)) {
    struct ramfs_node node;
    if (!ramfs_tmpfile(&dir->ramfs, mode, uid, gid, linkable, &node)) { return -28; }
    vfs_from_ramfs(&node, out);
    return 0;
  }
  return -95;
}

bool vfs_link_node(const struct vfs_node *node, const char *new_path) {
  vfs_update_time_sources();
  bool ok = false;
  if (node->backend == VFS_EXT2 && !vfs_path_uses_ramfs(new_path)) {
    ok = ext2_link_inode(vfs_root_ext2, node->ext2.ino, new_path);
  } else if (node->backend == VFS_RAMFS && node->ramfs.fs != NULL) {
    ok = ramfs_link_node(node->ramfs.fs, node->ramfs.index, new_path);
  }
  if (ok) { vfs_lookup_cache_clear(); fsnotify_linked(new_path); }
  return ok;
}

bool vfs_mkfifo(const char *path, uint32_t mode, struct vfs_node *out) {
  vfs_update_time_sources();
  struct ramfs_node node;
  if (vfs_root_ramfs == NULL || !vfs_path_uses_ramfs(path) ||
      !ramfs_mkfifo(vfs_root_ramfs, path, (uint16_t)mode, &node)) {
    return false;
  }
  vfs_from_ramfs(&node, out);
  vfs_lookup_cache_clear(); fsnotify_path_event(path, FS_CREATE);
  return true;
}

bool vfs_mksock(const char *path, uint32_t mode, struct vfs_node *out) {
  vfs_update_time_sources();
  struct ramfs_node node;
  if (vfs_root_ramfs == NULL || !vfs_path_uses_ramfs(path) ||
      !ramfs_mksock(vfs_root_ramfs, path, (uint16_t)mode, &node)) {
    return false;
  }
  vfs_from_ramfs(&node, out);
  vfs_lookup_cache_clear(); fsnotify_path_event(path, FS_CREATE);
  return true;
}

bool vfs_truncate(const struct vfs_node *node, uint64_t size) {
  vfs_update_time_sources();
  if (vfs_truncate_sealed(node, size)) { return false; }
  bool ok = false;
  if (node->backend == VFS_EXT2) {
    struct ext2_node ext;
    ok = ext2_inode(vfs_root_ext2, node->ext2.ino, &ext) && ext2_truncate(vfs_root_ext2, &ext, size);
  } else if (node->backend == VFS_RAMFS) {
    ok = ramfs_truncate(node->ramfs.fs, node->ramfs.index, size);
  }
  if (ok) {
    vfs_page_cache_truncate(node, size);
    vfs_lookup_cache_changed(node);
  }
  return ok;
}

int vfs_remove(const char *path) {
  struct fsnotify_entry old = {0};
  fsnotify_capture(path, &old);
  fsnotify_prepare_change(&old);
  struct vfs_node held;
  bool retained = fsnotify_hold_watched(path, &held);
  vfs_update_time_sources();
  int rc;
  if (vfs_root_ext2 != NULL && !vfs_path_uses_ramfs(path)) {
    rc = ext2_remove(vfs_root_ext2, path);
  } else {
    rc = vfs_root_ramfs != NULL ? ramfs_remove(vfs_root_ramfs, path) : -2;
  }
  if (rc == 0) { vfs_lookup_cache_clear(); fsnotify_removed(&old); }
  if (retained) { vfs_release_node(&held); }
  return rc;
}

bool vfs_link(const char *old_path, const char *new_path) {
  vfs_update_time_sources();
  bool ok = false;
  if (vfs_root_ext2 != NULL && !vfs_path_uses_ramfs(old_path) && !vfs_path_uses_ramfs(new_path)) {
    ok = ext2_link(vfs_root_ext2, old_path, new_path);
  } else if (vfs_root_ramfs != NULL && vfs_paths_share_ramfs(old_path, new_path)) {
    ok = ramfs_link(vfs_root_ramfs, old_path, new_path);
  }
  if (ok) { vfs_lookup_cache_clear(); fsnotify_linked(new_path); }
  return ok;
}

bool vfs_symlink(const char *target, const char *link_path) {
  vfs_update_time_sources();
  bool ok = vfs_root_ext2 != NULL && !vfs_path_uses_ramfs(link_path) && ext2_symlink(vfs_root_ext2, target, link_path);
  if (ok) { vfs_lookup_cache_clear(); fsnotify_path_event(link_path, FS_CREATE); }
  return ok;
}

bool vfs_chmod(const char *path, uint32_t mode) {
  struct vfs_node node;
  if (!vfs_lookup(path, &node) || !vfs_chmod_node(&node, mode)) { return false; }
  fsnotify_path_event(path, FS_ATTRIB);
  return true;
}

bool vfs_chmod_node(const struct vfs_node *node, uint32_t mode) {
  vfs_update_time_sources();
  bool ok = false;
  if (node->backend == VFS_EXT2) {
    ok = ext2_chmod_node(vfs_root_ext2, &node->ext2, mode);
  } else if (node->backend == VFS_RAMFS) {
    ok = ramfs_chmod_node(node->ramfs.fs, node->ramfs.index, (uint16_t)mode);
  }
  if (ok) { vfs_lookup_cache_changed(node); }
  return ok;
}

bool vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
  struct vfs_node node;
  if (!vfs_lookup(path, &node) || !vfs_chown_node(&node, uid, gid)) { return false; }
  fsnotify_path_event(path, FS_ATTRIB);
  return true;
}

bool vfs_chown_node(const struct vfs_node *node, uint32_t uid, uint32_t gid) {
  vfs_update_time_sources();
  bool ok = false;
  if (node->backend == VFS_EXT2) {
    ok = ext2_chown_node(vfs_root_ext2, &node->ext2, uid, gid);
  } else if (node->backend == VFS_RAMFS) {
    ok = ramfs_chown_node(node->ramfs.fs, node->ramfs.index, uid, gid);
  }
  if (ok) { vfs_lookup_cache_changed(node); }
  return ok;
}

int vfs_rename(const char *old_path, const char *new_path, bool noreplace) {
  struct fsnotify_entry old = {0}, to = {0};
  fsnotify_capture(old_path, &old);
  fsnotify_capture(new_path, &to);
  fsnotify_prepare_change(&old);
  fsnotify_prepare_change(&to);
  struct vfs_node held;
  bool retained = fsnotify_hold_watched(new_path, &held);
  vfs_update_time_sources();
  int rc = -18;
  if (vfs_root_ext2 && !vfs_path_uses_ramfs(old_path) && !vfs_path_uses_ramfs(new_path)) {
    rc = ext2_renameat(vfs_root_ext2, old_path, new_path, noreplace);
  } else if (vfs_root_ramfs && vfs_paths_share_ramfs(old_path, new_path)) {
    rc = ramfs_renameat(vfs_root_ramfs, old_path, new_path, noreplace);
  }
  if (!rc) { vfs_lookup_cache_clear(); fsnotify_renamed(&old, &to); }
  if (retained) { vfs_release_node(&held); }
  return rc;
}

bool vfs_retain_node(const struct vfs_node *node) {
  if (node->backend == VFS_EXT2) { return ext2_retain_inode(vfs_root_ext2, node->ext2.ino); }
  if (node->backend == VFS_RAMFS) { return ramfs_retain_node(node->ramfs.fs, node->ramfs.index); }
  return true;
}

void vfs_release_node(const struct vfs_node *node) {
  if (node->backend == VFS_EXT2) { ext2_release_inode(vfs_root_ext2, node->ext2.ino); }
  if (node->backend == VFS_RAMFS) { ramfs_release_node(node->ramfs.fs, node->ramfs.index); }
  inotify_flush();
}

bool vfs_set_sealable(const struct vfs_node *node, bool allow_sealing) {
  if (node == NULL || node->backend != VFS_RAMFS) { return false; }
  return ramfs_set_sealable(node->ramfs.fs, node->ramfs.index, allow_sealing);
}

int vfs_add_seals(const struct vfs_node *node, uint32_t seals) {
  if (node == NULL || node->backend != VFS_RAMFS) { return -22; }
  return ramfs_add_seals(node->ramfs.fs, node->ramfs.index, seals);
}

int vfs_get_seals(const struct vfs_node *node) {
  if (node == NULL || node->backend != VFS_RAMFS) { return -22; }
  return ramfs_get_seals(node->ramfs.fs, node->ramfs.index);
}

bool vfs_write_sealed(const struct vfs_node *node) {
  return node != NULL && node->backend == VFS_RAMFS && ramfs_write_sealed(node->ramfs.fs, node->ramfs.index);
}

bool vfs_truncate_sealed(const struct vfs_node *node, uint64_t size) {
  return node != NULL && node->backend == VFS_RAMFS && ramfs_truncate_sealed(node->ramfs.fs, node->ramfs.index, size);
}

void vfs_note_shared_writable_mapping(const struct vfs_node *node, bool add) {
  if (node == NULL || node->backend != VFS_RAMFS) { return; }
  ramfs_note_shared_writable_mapping(node->ramfs.fs, node->ramfs.index, add);
}

uint64_t vfs_read(const struct vfs_node *node, uint64_t off, void *dst, uint64_t len) {
  if (!vfs_node_cacheable(node) || len == 0 || off >= node->size) { return vfs_read_uncached(node, off, dst, len); }
  uint64_t available = node->size - off;
  if (len > available) { len = available; }
  return vfs_read_cached(node, off, dst, len);
}

static int64_t write_node(const struct vfs_node *node, uint64_t off, const void *src, uint64_t len, bool writeback) {
  vfs_update_time_sources();
  int64_t written = -28;
  if (node->backend == VFS_EXT2) {
    struct ext2_node ext;
    if (!ext2_inode(vfs_root_ext2, node->ext2.ino, &ext)) { return -5; }
    written = writeback ? ext2_writeback_file(vfs_root_ext2, &ext, off, src, len)
                        : ext2_write_file(vfs_root_ext2, &ext, off, src, len);
  } else if (node->backend == VFS_RAMFS) {
    written = writeback ? ramfs_writeback(node->ramfs.fs, node->ramfs.index, off, src, len)
                        : ramfs_write(node->ramfs.fs, node->ramfs.index, off, src, len);
  }
  if (written > 0) {
    vfs_page_cache_write(node, off, src, (uint64_t)written);
    vfs_lookup_cache_changed(node);
  }
  return written;
}

int64_t vfs_write(const struct vfs_node *node, uint64_t off, const void *src, uint64_t len) {
  return write_node(node, off, src, len, false);
}

int64_t vfs_writeback(const struct vfs_node *node, uint64_t off, const void *src, uint64_t len) {
  return write_node(node, off, src, len, true);
}

bool vfs_refresh(const struct vfs_node *node, struct vfs_node *out) {
  if (node->backend == VFS_RAMFS) {
    struct ramfs_node fresh;
    if (!ramfs_refresh_node(node->ramfs.fs, node->ramfs.index, &fresh)) { return false; }
    vfs_from_ramfs(&fresh, out);
    return true;
  }
  if (node->backend == VFS_EXT2) {
    struct ext2_node fresh;
    if (!ext2_inode(vfs_root_ext2, node->ext2.ino, &fresh)) { return false; }
    vfs_from_ext2(&fresh, out);
    return true;
  }
  if (node->backend == VFS_PROC) {
    *out = *node;
    return vfs_proc_node_live(node);
  }
  return false;
}
