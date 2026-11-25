#define _POSIX_C_SOURCE 200809L
#include "fs/ext2.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool read_at(void *ctx, uint64_t off, void *buf, uint32_t len) {
  return pread((int)(intptr_t)ctx, buf, len, off) == len;
}

static bool write_at(void *ctx, uint64_t off, const void *buf, uint32_t len) {
  return pwrite((int)(intptr_t)ctx, buf, len, off) == len;
}

struct ext2_fs *volume_open(int fd, bool writable) {
  struct ext2_fs *fs = calloc(1, sizeof(*fs));
  if (fs && ext2_mount(fs, read_at, (void *)(intptr_t)fd)) {
    if (writable) {
      fs->write = write_at;
      ext2_set_now((uint32_t)time(NULL));
    }
    return fs;
  }
  if (fs) { ext2_drop_cache(fs); }
  free(fs);
  return NULL;
}

void volume_close(struct ext2_fs *fs) {
  if (ext2_drop_cache(fs)) { free(fs); }
}

bool volume_copy_entry(struct ext2_fs *src, uint32_t ino, struct ext2_fs *dst, uint32_t parent, const char *path,
                       uint32_t *created) {
  struct ext2_node node, copy;
  if (!ext2_inode(src, ino, &node)) { return false; }
  if (ext2_is_symlink(&node)) {
    char *target = malloc((size_t)node.size + 1);
    size_t len;
    bool ok = target && ext2_readlink_node(src, &node, target, (size_t)node.size + 1, &len) && len == node.size &&
              strlen(target) == len && ext2_symlink_from(dst, parent, target, path);
    free(target);
    if (!ok || !ext2_lstat_from(dst, parent, path, &copy)) { return false; }
  } else {
    if (ext2_mknod_from(dst, parent, path, node.mode, &copy) != 0) { return false; }
    if (ext2_is_regular(&node)) {
      uint8_t buf[65536];
      for (uint64_t off = 0; off < node.size;) {
        uint32_t n = node.size - off > sizeof(buf) ? sizeof(buf) : node.size - off;
        uint32_t got;
        if (!ext2_read_file(src, &node, off, buf, n, &got) || got != n ||
            ext2_write_file(dst, &copy, off, buf, n) != n) {
          return false;
        }
        off += n;
      }
    }
  }
  *created = copy.ino;
  return ext2_chown_node(dst, &copy, node.uid, node.gid) && ext2_chmod_node(dst, &copy, node.mode) &&
         ext2_set_times(dst, copy.ino, node.atime, node.mtime);
}
