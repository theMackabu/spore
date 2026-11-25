#include "internal.h"

#if __STDC_HOSTED__ || defined(SPORE_DISK_TOOL)
#include "mm/table.h"
#else
#include "mm/pool.h"
static struct object_pool ref_pool;
#endif

struct inode_ref {
  struct inode_ref *next;
  struct ext2_fs *fs;
  uint32_t ino;
  size_t count;
  bool orphan;
  bool linkable;
};

static struct inode_ref *buckets[64];

static struct inode_ref **find_ref(struct ext2_fs *fs, uint32_t ino) {
  struct inode_ref **slot = &buckets[(ino ^ (ino >> 6)) % 64];
  while (*slot && ((*slot)->ino != ino || (*slot)->fs != fs)) {
    slot = &(*slot)->next;
  }
  return slot;
}

bool ext2_retain_inode(struct ext2_fs *fs, uint32_t ino) {
  struct inode_ref **slot = find_ref(fs, ino);
  if (*slot) {
    ++(*slot)->count;
    return true;
  }
#if __STDC_HOSTED__ || defined(SPORE_DISK_TOOL)
  struct inode_ref *ref = table_alloc(1, sizeof(*ref));
#else
  if (!ref_pool.limit && !pool_init(&ref_pool, sizeof(struct inode_ref), SIZE_MAX)) { return false; }
  struct inode_ref *ref = pool_alloc(&ref_pool);
#endif
  if (!ref) { return false; }
  *ref = (struct inode_ref){.fs = fs, .ino = ino, .count = 1};
  *slot = ref;
  return true;
}

static bool reap(struct inode_ref **slot) {
  struct inode_ref *ref = *slot;
  if (ref->orphan) {
    struct ext2_node node;
    if (!ext2_read_inode(ref->fs, ref->ino, &node) || node.links_count || !ext2_truncate(ref->fs, &node, 0) ||
        !ext2_free_inode(ref->fs, ref->ino)) {
      return false;
    }
  }
  *slot = ref->next;
#if __STDC_HOSTED__ || defined(SPORE_DISK_TOOL)
  table_free(ref, 1, sizeof(*ref));
#else
  pool_free(&ref_pool, ref);
#endif
  return true;
}

void ext2_release_inode(struct ext2_fs *fs, uint32_t ino) {
  struct inode_ref **slot = find_ref(fs, ino);
  if (*slot && (*slot)->count && --(*slot)->count == 0) { (void)reap(slot); }
}

bool ext2_check_refs(struct ext2_fs *fs, bool remount) {
  bool ok = true;
  for (size_t i = 0; i < sizeof(buckets) / sizeof(*buckets); ++i) {
    struct inode_ref **slot = &buckets[i];
    while (*slot) {
      if ((*slot)->fs == fs) {
        if (!(*slot)->count && reap(slot)) { continue; }
        if (remount || !(*slot)->count) { ok = false; }
      }
      slot = &(*slot)->next;
    }
  }
  return ok;
}

bool ext2_shutdown(struct ext2_fs *fs) {
  bool ok = true;
  for (size_t i = 0; i < sizeof(buckets) / sizeof(*buckets); ++i) {
    struct inode_ref **slot = &buckets[i];
    while (*slot) {
      if ((*slot)->fs == fs && (*slot)->orphan) {
        if (reap(slot)) { continue; }
        ok = false;
      }
      slot = &(*slot)->next;
    }
  }
  return ext2_flush(fs) && ok;
}

bool ext2_set_orphan(struct ext2_fs *fs, uint32_t ino, bool orphan, bool linkable) {
  struct inode_ref *ref = *find_ref(fs, ino);
  if (!ref) { return false; }
  ref->orphan = orphan;
  ref->linkable = linkable;
  return true;
}

bool ext2_inode_linkable(struct ext2_fs *fs, uint32_t ino) {
  struct inode_ref *ref = *find_ref(fs, ino);
  return ref && ref->linkable;
}

bool ext2_tmpfile(struct ext2_fs *fs, uint16_t mode, uint32_t uid, uint32_t gid, bool linkable, struct ext2_node *out) {
  uint32_t ino;
  if (fs->write == NULL || uid > UINT16_MAX || gid > UINT16_MAX || !ext2_alloc_inode(fs, &ino)) { return false; }
  uint32_t now = ext2_now_sec();
  struct ext2_node node = {
    .ino = ino, .mode = EXT2_S_IFREG | mode, .uid = uid, .gid = gid, .atime = now, .ctime = now, .mtime = now};
  if (!ext2_write_inode(fs, &node) || !ext2_retain_inode(fs, ino)) {
    (void)ext2_free_inode(fs, ino);
    return false;
  }
  ext2_set_orphan(fs, ino, true, linkable);
  *out = node;
  return true;
}
