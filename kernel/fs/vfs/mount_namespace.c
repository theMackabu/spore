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
  bool locked;
  char target[CELL_PATH_MAX];
  struct vfs_mount info;
};

struct mount_namespace {
  unsigned refs;
  struct user_namespace *owner;
  struct mount_entry *mounts;
  struct mount_namespace *next;
};

static struct mount_namespace initial;
static struct mount_namespace *namespaces = &initial;
static struct object_pool namespace_pool, mount_pool, fs_pool;
static size_t live_entries;
static uint32_t next_device = 0x10000;

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
  --live_entries;
  mountns_put_fs(m->fs);
  pool_free(&mount_pool, m);
}

void mountns_release(struct mount_namespace *ns) {
  if (!ns || --ns->refs) { return; }
  struct mount_namespace **slot = &namespaces;
  while (*slot != ns) { slot = &(*slot)->next; }
  *slot = ns->next;
  while (ns->mounts) {
    struct mount_entry *m = ns->mounts;
    ns->mounts = m->next;
    drop_entry(m);
  }
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
  ns->refs = 1;
  ns->owner = userns_retain(d->user_ns);
  ns->next = namespaces;
  namespaces = ns;
  struct mount_entry **slot = &ns->mounts;
  for (struct mount_entry *m = original->mounts; m; m = m->next) {
    struct mount_entry *copy = allocate(&mount_pool, sizeof(*copy));
    if (!copy) { mountns_release(ns); return -12; }
    *copy = *m;
    copy->locked |= ns->owner != original->owner;
    copy->next = NULL;
    copy->info.target = copy->target;
    ++live_entries;
    ++copy->fs->refs;
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

struct ramfs *mountns_lookup(const char *path, const char **local) {
  if (!live_entries) { return NULL; }
  for (struct mount_entry *m = view(cell_current_domain_internal())->mounts; m; m = m->next) {
    if (inside(path, m->target)) {
      const char *suffix = path + kstrlen(m->target);
      *local = *suffix ? suffix : "/";
      return m->fs;
    }
  }
  return NULL;
}

bool mountns_pinned_path(const char *path) {
  if (!live_entries) { return false; }
  struct vfs_node node;
  if (!vfs_lstat(path, &node) || !node.is_dir) { return false; }
  struct mount_namespace *current = view(cell_current_domain_internal());
  for (struct mount_namespace *ns = namespaces; ns; ns = ns->next) {
    for (struct mount_entry *m = ns->mounts; m; m = m->next) {
      if ((ns == current || node.dev_id == m->info.parent) && inside(m->target, path)) { return true; }
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
  m->info = (struct vfs_mount){fs->device, covered.dev_id, 0x01021994, "tmpfs", m->target, "tmpfs",
    nosuid ? (nodev ? "rw,nosuid,nodev" : "rw,nosuid") : (nodev ? "rw,nodev" : "rw")};
  m->next = ns->mounts;
  ns->mounts = m;
  ++live_entries;
  return 0;
}

int mountns_unmount(const char *path, bool detach) {
  struct mount_namespace *ns = view(cell_current_domain_internal());
  const char *local;
  if (!mountns_lookup(path, &local) || !str_eq(local, "/")) { return -22; }
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
