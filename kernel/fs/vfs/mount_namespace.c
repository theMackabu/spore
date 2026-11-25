#include "fs/mount_namespace.h"
#include "internal.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "mm/pool.h"
#include "proc/domain.h"
#include "proc/thread.h"
#include "proc/user_namespace.h"

struct mount_entry {
  struct mount_entry *next;
  struct ramfs *fs;
  int root;
  bool locked;
  char target[CELL_PATH_MAX];
  struct vfs_mount info;
};

struct mount_namespace {
  uint64_t ino;
  unsigned refs;
  struct user_namespace *owner;
  struct mount_entry *mounts;
  struct mount_namespace *next;
};

static struct mount_namespace initial;
static struct mount_namespace *namespaces = &initial;
uint64_t mountns_ino(const struct mount_namespace *ns) { return ns->ino; }

static struct object_pool namespace_pool, mount_pool, fs_pool;
static size_t live_entries;
static uint32_t next_device = 0x10000;
static uint64_t next_mount = 0x10000;

static void *allocate(struct object_pool *pool, size_t size) {
  if (!pool->limit && !pool_init(pool, size, SIZE_MAX)) { return NULL; }
  return pool_alloc(pool);
}

static struct mount_namespace *view(const struct domain *domain) {
  return domain && domain->mount_ns ? domain->mount_ns : &initial;
}

static bool inside(const char *path, const char *dir) {
  if (!starts_with(path, dir)) { return false; }
  char end = path[kstrlen(dir)];
  return !end || end == '/' || !dir[1];
}

struct mount_namespace *mountns_retain(struct mount_namespace *ns) {
  if (ns) { ++ns->refs; }
  return ns;
}

void mountns_put_fs(struct ramfs *fs) {
  if (fs->device && !--fs->refs) {
    ramfs_destroy(fs);
    pool_free(&fs_pool, fs);
  }
}

static void drop_entry(struct mount_entry *m) {
  if (m->root) { ramfs_release_node(m->fs, m->root); }
  --live_entries;
  mountns_put_fs(m->fs);
  pool_free(&mount_pool, m);
}

static struct mount_entry *copy_entry(const struct mount_entry *source) {
  struct mount_entry *copy = allocate(&mount_pool, sizeof(*copy));
  if (!copy) { return NULL; }
  *copy = *source;
  copy->next = NULL;
  copy->info.target = copy->target;
  if (copy->root && !ramfs_retain_node(copy->fs, copy->root)) {
    pool_free(&mount_pool, copy);
    return NULL;
  }
  ++copy->fs->refs;
  ++live_entries;
  return copy;
}

static void drop_entries(struct mount_entry *entry) {
  while (entry) {
    struct mount_entry *next = entry->next;
    drop_entry(entry);
    entry = next;
  }
}

void mountns_release(struct mount_namespace *ns) {
  if (!ns || --ns->refs) { return; }
  struct mount_namespace **slot = &namespaces;
  while (*slot != ns) { slot = &(*slot)->next; }
  *slot = ns->next;
  drop_entries(ns->mounts);
  userns_release(ns->owner);
  pool_free(&namespace_pool, ns);
}

int mountns_unshare(struct thread *thread) {
  struct domain *d = thread->domain;
  if (!(thread->capabilities.effective & (1u << CAP_SYS_ADMIN))) { return -1; }
  if (d->refcount != 1) { return -95; }
  const struct mount_namespace *original = view(d);
  struct mount_namespace *ns = allocate(&namespace_pool, sizeof(*ns));
  if (!ns) { return -12; }
  ns->ino = namespace_new_ino();
  ns->refs = 1;
  ns->owner = userns_retain(d->user_ns);
  ns->next = namespaces;
  namespaces = ns;
  struct mount_entry **slot = &ns->mounts;
  for (struct mount_entry *m = original->mounts; m; m = m->next) {
    struct mount_entry *copy = copy_entry(m);
    if (!copy) { mountns_release(ns); return -12; }
    copy->locked |= ns->owner != original->owner;
    *slot = copy;
    slot = &copy->next;
  }
  mountns_release(d->mount_ns);
  d->mount_ns = ns;
  return 0;
}

bool mountns_has_mounts(void) { return live_entries != 0; }

bool mountns_may_mount(void) {
  return cell_ns_capable(view(cell_current_domain_internal())->owner, CAP_SYS_ADMIN);
}

const struct vfs_mount *mountns_at(const struct domain *d, size_t index) {
  struct mount_entry *m = view(d)->mounts;
  while (m && index--) { m = m->next; }
  return m ? &m->info : NULL;
}

struct ramfs_route mountns_lookup(const char *path) {
  if (live_entries) {
    for (struct mount_entry *m = view(cell_current_domain_internal())->mounts; m; m = m->next) {
      if (inside(path, m->target)) {
        const char *suffix = path + kstrlen(m->target);
        return (struct ramfs_route){m->fs, *suffix ? suffix : "/", m->info.mount_id, m->root};
      }
    }
  }
  return (struct ramfs_route){.path = path};
}

const char *mountns_root_path(const struct vfs_mount *mount, char *buf, size_t size) {
  if (!mount->mount_id) { return "/"; }
  const struct mount_entry *m = (const void *)((const char *)mount - offsetof(struct mount_entry, info));
  return ramfs_inode_path(m->fs, m->root, buf, size);
}

bool mountns_pinned_path(const char *path) {
  if (!live_entries) { return false; }
  struct vfs_node node;
  if (!vfs_lstat(path, &node) || !node.is_dir) { return false; }
  struct mount_namespace *current = view(cell_current_domain_internal());
  for (struct mount_namespace *ns = namespaces; ns; ns = ns->next) {
    for (struct mount_entry *m = ns->mounts; m; m = m->next) {
      if ((ns == current || (node.mount_id ? node.mount_id : node.dev_id) == m->info.parent) && inside(m->target, path)) { return true; }
    }
  }
  return false;
}

int mountns_tmpfs(const char *path, unsigned mode, bool nosuid, bool nodev) {
  struct domain *domain = cell_current_domain_internal();
  struct mount_namespace *ns = view(domain);
  struct vfs_node covered;
  if (!vfs_lstat(path, &covered)) { return -2; }
  if (!covered.is_dir) { return -20; }
  if (!path[1] || vfs_readonly_path(path)) { return -95; }
  if (ns->owner && covered.uid != domain->euid && !str_eq(path, "/tmp") && !str_eq(path, "/run")) { return -1; }
  if (ns->owner) { nosuid = nodev = true; }
  if (!next_device) { return -28; }
  struct mount_entry *m = allocate(&mount_pool, sizeof(*m));
  if (!m) { return -12; }
  struct ramfs *fs = allocate(&fs_pool, sizeof(*fs));
  if (!fs || !ramfs_init_tmpfs(fs, vfs_root_ramfs->hhdm_offset, mode, domain->euid, domain->egid)) {
    if (fs) { pool_free(&fs_pool, fs); }
    pool_free(&mount_pool, m);
    return -12;
  }
  fs->refs = 1;
  fs->nosuid = nosuid;
  fs->device = next_device++;
  ramfs_set_reclaim_callback(fs, vfs_ramfs_inode_freed);
  m->fs = fs;
  copy_cstr(m->target, sizeof(m->target), path);
  m->info = (struct vfs_mount){fs->device, covered.mount_id ? covered.mount_id : covered.dev_id, 0x01021994, "tmpfs", m->target, "tmpfs",
    nosuid ? (nodev ? "rw,nosuid,nodev" : "rw,nosuid") : (nodev ? "rw,nodev" : "rw"), next_mount++};
  m->next = ns->mounts;
  ns->mounts = m;
  ++live_entries;
  return 0;
}

static bool mounted_below(const struct mount_entry *child, const struct mount_entry *ancestor) {
  while (child->info.mount_id != ancestor->info.mount_id) {
    uint64_t parent = child->info.parent;
    do { child = child->next; } while (child && child->info.mount_id != parent);
    if (!child) { return false; }
  }
  return true;
}

static int attach_bind_tree(struct mount_namespace *ns, struct mount_entry *root, const struct mount_entry *origin,
                            const char *source, bool recursive) {
  struct mount_entry *tree = root;
  struct mount_entry **tail = &tree;
  size_t src_len = kstrlen(source), dst_len = kstrlen(root->target);
  if (recursive) {
    for (struct mount_entry *m = ns->mounts; m != origin; m = m->next) {
      if (!inside(m->target, source) || !mounted_below(m, origin)) { continue; }
      const char *suffix = m->target + src_len;
      size_t len = kstrlen(suffix);
      if (dst_len + len >= CELL_PATH_MAX) { drop_entries(tree); return -36; }
      struct mount_entry *copy = copy_entry(m);
      if (!copy) { drop_entries(tree); return -12; }
      kmemcpy(copy->target, root->target, dst_len);
      kmemcpy(copy->target + dst_len, suffix, len + 1);
      copy->next = root;
      *tail = copy;
      tail = &copy->next;
    }
  }
  for (struct mount_entry *m = tree; m; m = m->next) {
    uint64_t old = m->info.mount_id;
    m->info.mount_id = next_mount++;
    for (struct mount_entry *child = tree; child != m; child = child->next) {
      if (child->info.parent == old) { child->info.parent = m->info.mount_id; }
    }
  }
  root->next = ns->mounts;
  ns->mounts = tree;
  return 0;
}

int mountns_bind(const char *source, const char *target, bool recursive) {
  struct domain *d = cell_current_domain_internal();
  if (cell_fs_restricted()) { return -1; }
  struct mount_namespace *ns = view(d);
  struct vfs_node src, dst;
  if (!vfs_lookup(source, &src) || !vfs_lookup(target, &dst)) { return -2; }
  if (!src.is_dir || !dst.is_dir) { return -20; }
  if (src.backend != VFS_RAMFS || !src.ramfs.fs->device || !target[1]) { return -95; }
  if (ns->owner && dst.uid != d->euid) { return -1; }
  struct mount_entry *origin = NULL;
  for (struct mount_entry *m = ns->mounts; m; m = m->next) {
    if (m->info.mount_id == src.mount_id) { origin = m; break; }
  }
  if (!origin) { return -22; }
  struct mount_entry *m = allocate(&mount_pool, sizeof(*m));
  if (!m) { return -12; }
  m->root = src.ramfs.index;
  m->fs = src.ramfs.fs;
  if (m->root && !ramfs_retain_node(m->fs, m->root)) { pool_free(&mount_pool, m); return -2; }
  ++m->fs->refs;
  copy_cstr(m->target, sizeof(m->target), target);
  m->info = origin->info;
  m->info.parent = dst.mount_id ? dst.mount_id : dst.dev_id;
  m->info.target = m->target;
  ++live_entries;
  return attach_bind_tree(ns, m, origin, source, recursive);
}

int mountns_unmount(const char *path, bool detach) {
  struct mount_namespace *ns = view(cell_current_domain_internal());
  struct ramfs_route routed = mountns_lookup(path);
  if (!routed.fs || !str_eq(routed.path, "/")) { return -22; }
  struct mount_entry **slot = &ns->mounts;
  while (*slot && !str_eq((*slot)->target, path)) {
    if (!detach && inside((*slot)->target, path)) { return -16; }
    slot = &(*slot)->next;
  }
  if (!*slot) { return -22; }
  struct mount_entry *m = *slot;
  if (m->locked) { return -1; }
  if (!detach && m->fs->pins) { return -16; }
  if (detach) {
    slot = &ns->mounts;
    while (*slot != m) {
      struct mount_entry *child = *slot;
      if (inside(child->target, path)) {
        *slot = child->next;
        drop_entry(child);
      } else { slot = &child->next; }
    }
  }
  *slot = m->next;
  drop_entry(m);
  return 0;
}
