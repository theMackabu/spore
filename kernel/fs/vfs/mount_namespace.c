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
  struct mount_access *access;
  const struct vfs_mount *boot;
  bool locked;
  char target[CELL_PATH_MAX];
  struct vfs_mount info;
};

struct mount_namespace {
  uint64_t ino;
  unsigned refs;
  bool explicit_mounts;
  struct user_namespace *owner;
  struct mount_entry *mounts;
  struct mount_entry *stacked;
  struct mount_namespace *next;
};

static struct mount_namespace initial;
static struct mount_namespace *namespaces = &initial;
uint64_t mountns_ino(const struct mount_namespace *ns) { return ns->ino; }

static struct object_pool namespace_pool, mount_pool, fs_pool, access_pool;
static size_t live_entries;
static uint32_t next_device = 0x10000;
static uint64_t next_mount = 0x10000;

static void *allocate(struct object_pool *pool, size_t size) {
  if (!pool->limit && !pool_init(pool, size, SIZE_MAX)) { return NULL; }
  return pool_alloc(pool);
}

static struct mount_access *new_access(unsigned flags) {
  struct mount_access *access = allocate(&access_pool, sizeof(*access));
  if (access) { *access = (struct mount_access){.refs = 1, .flags = flags}; }
  return access;
}

void mountns_access_put(struct mount_access *access) {
  if (access && !--access->refs) { pool_free(&access_pool, access); }
}

static const char *options_for(unsigned flags) {
  static const char *const options[] = {"rw", "ro", "rw,nosuid", "ro,nosuid", "rw,nodev", "ro,nodev",
                                         "rw,nosuid,nodev", "ro,nosuid,nodev"};
  return options[flags];
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
  if (m->root && !m->boot) { ramfs_release_node(m->fs, m->root); }
  mountns_access_put(m->access);
  --live_entries;
  if (!m->boot) { mountns_put_fs(m->fs); }
  pool_free(&mount_pool, m);
}

static struct mount_entry *copy_entry(const struct mount_entry *source, bool separate) {
  struct mount_entry *copy = allocate(&mount_pool, sizeof(*copy));
  if (!copy) { return NULL; }
  *copy = *source;
  copy->next = NULL;
  copy->info.target = copy->target;
  if (copy->root && !copy->boot && !ramfs_retain_node(copy->fs, copy->root)) {
    pool_free(&mount_pool, copy);
    return NULL;
  }
  if (separate) {
    copy->access = new_access(source->access->flags);
    if (!copy->access) {
      if (!copy->boot && copy->root) { ramfs_release_node(copy->fs, copy->root); }
      pool_free(&mount_pool, copy);
      return NULL;
    }
  } else { ++copy->access->refs; }
  if (!copy->boot) { ++copy->fs->refs; }
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
  drop_entries(ns->stacked);
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
  ns->explicit_mounts = original->explicit_mounts;
  ns->owner = userns_retain(d->user_ns);
  ns->next = namespaces;
  namespaces = ns;
  for (unsigned hidden = 0; hidden < 2; ++hidden) {
    struct mount_entry **slot = hidden ? &ns->stacked : &ns->mounts;
    for (struct mount_entry *m = hidden ? original->stacked : original->mounts; m; m = m->next) {
      struct mount_entry *copy = copy_entry(m, true);
      if (!copy) { mountns_release(ns); return -12; }
      copy->locked |= ns->owner != original->owner;
      *slot = copy;
      slot = &copy->next;
    }
  }
  mountns_release(d->mount_ns);
  d->mount_ns = ns;
  return 0;
}

bool mountns_explicit(const struct domain *d) { return view(d)->explicit_mounts; }

bool mountns_current_routes(void) { return live_entries && view(cell_current_domain_internal())->mounts; }

bool mountns_has_mounts(void) { return live_entries != 0; }

bool mountns_may_mount(void) {
  return cell_ns_capable(view(cell_current_domain_internal())->owner, CAP_SYS_ADMIN);
}

const struct vfs_mount *mountns_at(const struct domain *d, size_t index) {
  const struct mount_namespace *ns = view(d);
  for (unsigned hidden = 0; hidden < 2; ++hidden) {
    struct mount_entry *m = hidden ? ns->stacked : ns->mounts;
    size_t count = 0;
    for (struct mount_entry *it = m; it; it = it->next) { ++count; }
    if (index < count) {
      for (size_t skip = count - index - 1; skip; --skip) { m = m->next; }
      return &m->info;
    }
    index -= count;
  }
  return NULL;
}

struct ramfs_route mountns_lookup(const char *path) {
  if (live_entries) {
    for (struct mount_entry *m = view(cell_current_domain_internal())->mounts; m; m = m->next) {
      if (inside(path, m->target)) {
        const char *suffix = m->target[1] ? path + kstrlen(m->target) : path;
        return (struct ramfs_route){m->fs, *suffix ? suffix : "/", m->info.mount_id, m->root, m->boot, m->access};
      }
    }
  }
  return (struct ramfs_route){.path = path};
}

const char *mountns_root_path(const struct vfs_mount *mount, char *buf, size_t size) {
  if (!mount->mount_id) { return "/"; }
  const struct mount_entry *m = (const void *)((const char *)mount - offsetof(struct mount_entry, info));
  if (m->boot) { return "/"; }
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
  m->access = new_access((nosuid ? MOUNT_NOSUID : 0) | (nodev ? MOUNT_NODEV : 0));
  if (!m->access) { mountns_put_fs(fs); pool_free(&mount_pool, m); return -12; }
  m->fs = fs;
  copy_cstr(m->target, sizeof(m->target), path);
  m->info = (struct vfs_mount){fs->device, covered.mount_id ? covered.mount_id : covered.dev_id, 0x01021994, "tmpfs", m->target, "tmpfs",
    options_for(m->access->flags), next_mount++};
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
      struct mount_entry *copy = copy_entry(m, true);
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
  if (!target[1]) { return -95; }
  if (ns->owner && dst.uid != d->euid) { return -1; }
  struct mount_entry *origin = NULL;
  for (struct mount_entry *m = ns->mounts; m; m = m->next) {
    if (m->info.mount_id == src.mount_id) { origin = m; break; }
  }
  if (!origin) { return -95; }
  if (origin->boot && !str_eq(mountns_lookup(source).path, "/")) { return -95; }
  struct mount_entry *m = allocate(&mount_pool, sizeof(*m));
  if (!m) { return -12; }
  m->access = new_access(origin->access->flags);
  if (!m->access) { pool_free(&mount_pool, m); return -12; }
  m->boot = origin->boot;
  m->root = origin->boot ? origin->root : src.ramfs.index;
  m->fs = origin->fs;
  if (!m->boot && m->root && !ramfs_retain_node(m->fs, m->root)) { mountns_access_put(m->access); pool_free(&mount_pool, m); return -2; }
  if (!m->boot) { ++m->fs->refs; }
  copy_cstr(m->target, sizeof(m->target), target);
  m->info = origin->info;
  m->info.parent = dst.mount_id ? dst.mount_id : dst.dev_id;
  m->info.target = m->target;
  ++live_entries;
  return attach_bind_tree(ns, m, origin, source, recursive);
}

int mountns_remount(const char *path, unsigned flags) {
  struct ramfs_route route = mountns_lookup(path);
  if (!route.mount_id || !str_eq(route.path, "/")) { return -22; }
  struct mount_namespace *ns = view(cell_current_domain_internal());
  for (struct mount_entry *m = ns->mounts; m; m = m->next) {
    if (m->info.mount_id != route.mount_id) { continue; }
    if (ns->owner && (m->access->flags & ~flags)) { return -1; }
    if (m->boot && str_eq(m->boot->options, "ro") && !(flags & MOUNT_RDONLY)) { return -30; }
    if ((flags & MOUNT_RDONLY) && m->access->writers) { return -16; }
    m->access->flags = flags;
    m->info.options = options_for(flags);
    return 0;
  }
  return -22;
}

int mountns_unmount(const char *path, bool detach) {
  struct mount_namespace *ns = view(cell_current_domain_internal());
  struct ramfs_route routed = mountns_lookup(path);
  if (!path[1]) { return -16; }
  if (!routed.mount_id || !str_eq(routed.path, "/")) { return -22; }
  struct mount_entry **slot = &ns->mounts;
  while (*slot && !str_eq((*slot)->target, path)) {
    if (!detach && inside((*slot)->target, path)) { return -16; }
    slot = &(*slot)->next;
  }
  if (!*slot) { return -22; }
  struct mount_entry *m = *slot;
  if (m->locked) { return -1; }
  if (!detach && (m->boot || m->fs->pins)) { return -16; }
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

static bool join_path(char *dst, size_t cap, const char *base, const char *suffix) {
  size_t a = base[1] ? kstrlen(base) : 0;
  size_t b = suffix[1] ? kstrlen(suffix) : 0;
  if (a + b >= cap || cap < 2) { return false; }
  kmemcpy(dst, base, a);
  kmemcpy(dst + a, suffix, b);
  if (!a && !b) { dst[a++] = '/'; }
  dst[a + b] = 0;
  return true;
}

static struct mount_entry *boot_tree(void) {
  struct mount_entry *tree = NULL;
  const struct vfs_mount *boot;
  for (size_t i = 0; (boot = vfs_boot_mount_at(i)); ++i) {
    struct mount_entry *m = allocate(&mount_pool, sizeof(*m));
    if (!m) { drop_entries(tree); return NULL; }
    m->boot = boot;
    if (boot->device != VFS_DEV_EXT2_ROOT) {
      struct ramfs_node n;
      if (!ramfs_lookup_node(vfs_root_ramfs, 0, boot->target, &n)) {
        pool_free(&mount_pool, m);
        drop_entries(tree);
        return NULL;
      }
      m->root = n.index;
      m->fs = vfs_root_ramfs;
    }
    copy_cstr(m->target, sizeof(m->target), boot->target);
    m->info = *boot;
    m->info.mount_id = boot->device;
    m->info.target = m->target;
    m->access = new_access(str_eq(boot->options, "ro") ? MOUNT_RDONLY : 0);
    if (!m->access) { pool_free(&mount_pool, m); drop_entries(tree); return NULL; }
    m->locked = true;
    struct mount_entry **slot = &tree;
    while (*slot && kstrlen((*slot)->target) >= kstrlen(m->target)) { slot = &(*slot)->next; }
    m->next = *slot;
    *slot = m;
    ++live_entries;
  }
  return tree;
}

static int pivot_tree(struct mount_namespace *ns, const struct mount_entry *root, const char *put_old,
                      struct mount_entry **result, struct mount_entry **stacked) {
  struct mount_entry *base = ns->explicit_mounts ? NULL : boot_tree();
  if (!ns->explicit_mounts && !base) { return -12; }
  struct mount_entry *old_tree = NULL, **old_tail = &old_tree;
  struct mount_entry *new_tree = NULL, **new_tail = &new_tree;
  int rc = 0;
  for (unsigned part = 0; part < 2 && !rc; ++part) {
    for (struct mount_entry *m = part ? base : ns->mounts; m; m = m->next) {
      struct mount_entry *copy = copy_entry(m, false);
      if (!copy) { rc = -12; break; }
      if (!part && mounted_below(m, root)) {
        const char *suffix = m->target + kstrlen(root->target);
        copy_cstr(copy->target, sizeof(copy->target), *suffix ? suffix : "/");
        if (m == root) { copy->info.parent = 0; }
        *new_tail = copy;
        new_tail = &copy->next;
      } else {
        *old_tail = copy;
        old_tail = &copy->next;
        if (!join_path(copy->target, sizeof(copy->target), put_old, m->target)) { rc = -36; break; }
        if (!m->target[1]) {
          copy->info.parent = root->info.mount_id;
          copy->locked = false;
        }
      }
    }
  }
  drop_entries(base);
  if (rc) { drop_entries(old_tree); drop_entries(new_tree); return rc; }
  if (stacked) {
    *stacked = old_tree;
    *result = new_tree;
  } else {
    *old_tail = new_tree;
    *result = old_tree;
  }
  return 0;
}

static bool pivot_cwd(const char *cwd, const char *root, const char *old, char path[CELL_PATH_MAX]) {
  if (!cwd[1]) { copy_cstr(path, CELL_PATH_MAX, "/"); return true; }
  if (!inside(cwd, root)) { return join_path(path, CELL_PATH_MAX, old, cwd); }
  const char *suffix = cwd + kstrlen(root);
  copy_cstr(path, CELL_PATH_MAX, *suffix ? suffix : "/");
  return true;
}

int mountns_pivot(const char *root_path, const char *old_path) {
  struct domain *caller = cell_current_domain_internal();
  struct mount_namespace *ns = view(caller);
  if (!mountns_may_mount() || cell_fs_restricted()) { return -1; }
  if (ns == &initial || !root_path[1] || !inside(old_path, root_path)) { return -22; }
  if (ns->stacked) { return -16; }
  bool stack = str_eq(root_path, old_path);
  struct ramfs_route new = mountns_lookup(root_path), old = mountns_lookup(old_path);
  if (!new.mount_id || !str_eq(new.path, "/") || old.mount_id != new.mount_id) { return -22; }
  struct vfs_node dir;
  if (!vfs_lstat(old_path, &dir)) { return -2; }
  if (!dir.is_dir) { return -20; }
  struct mount_entry *root = ns->mounts;
  while (root && root->info.mount_id != new.mount_id) { root = root->next; }
  if (!root || root->locked) { return -22; }
  const char *put_old = stack ? "/" : old_path + kstrlen(root_path);
  char cwd[CELL_PATH_MAX];
  for (size_t i = 0; i < cell_domain_slot_limit(); ++i) {
    struct domain *d = cell_domain_slot(i);
    if (!d->used || view(d) != ns) { continue; }
    if (d->chroot[1]) { return -22; }
    if (!pivot_cwd(d->cwd, root_path, put_old, cwd)) { return -36; }
  }
  struct mount_entry *tree, *stacked = NULL;
  int rc = pivot_tree(ns, root, put_old, &tree, stack ? &stacked : NULL);
  if (rc) { return rc; }
  struct mount_entry *previous = ns->mounts;
  ns->mounts = tree;
  ns->stacked = stacked;
  ns->explicit_mounts = true;
  for (size_t i = 0; i < cell_domain_slot_limit(); ++i) {
    struct domain *d = cell_domain_slot(i);
    if (!d->used || view(d) != ns) { continue; }
    if (stack && d->cwd[1] && !inside(d->cwd, root_path)) {
      for (struct mount_entry *m = stacked; m; m = m->next) {
        if (inside(d->cwd, m->target)) { d->cwd_covered_mount = m->info.mount_id; break; }
      }
    }
    pivot_cwd(d->cwd, root_path, put_old, cwd);
    copy_cstr(d->cwd, sizeof(d->cwd), cwd);
  }
  drop_entries(previous);
  return 0;
}

int mountns_relocate_dir(const struct vfs_node *node, const char *opened, char *path, size_t cap) {
  struct mount_namespace *ns = view(cell_current_domain_internal());
  if (!ns->explicit_mounts) { return -116; }
  uint64_t id = node->mount_id ? node->mount_id : node->dev_id;
  for (struct mount_entry *m = ns->mounts; m; m = m->next) {
    if (m->info.mount_id != id) { continue; }
    char absolute[CELL_PATH_MAX], prefix[CELL_PATH_MAX];
    const char *suffix;
    if (node->backend == VFS_RAMFS && m->fs == node->ramfs.fs) {
      const char *name = ramfs_inode_path(m->fs, node->ramfs.index, absolute, sizeof(absolute));
      const char *base = ramfs_inode_path(m->fs, m->root, prefix, sizeof(prefix));
      if (!name || !base || !inside(name, base)) { return -116; }
      suffix = base[1] ? name + kstrlen(base) : name;
    } else if (m->boot && inside(opened, m->boot->target)) {
      suffix = m->boot->target[1] ? opened + kstrlen(m->boot->target) : opened;
    } else { return -116; }
    return join_path(path, cap, m->target, *suffix ? suffix : "/") ? 0 : -36;
  }
  if (node->dev_id == VFS_DEV_PROC) {
    unsigned required = node->access ? node->access->flags : 0;
    for (struct mount_entry *m = ns->mounts; m; m = m->next) {
      if (!m->boot || m->boot->device != VFS_DEV_PROC || (required & ~m->access->flags) ||
          !inside(opened, m->boot->target)) { continue; }
      const char *suffix = opened + kstrlen(m->boot->target);
      if (!join_path(path, cap, m->target, *suffix ? suffix : "/")) { continue; }
      struct vfs_node named;
      if (vfs_lookup(path, &named) && named.mount_id == m->info.mount_id && named.ino == node->ino &&
          named.dev_id == node->dev_id) { return 0; }
    }
  }
  return -116;
}


bool mountns_covered_root(const struct vfs_node *node) {
  const struct mount_namespace *ns = view(cell_current_domain_internal());
  uint64_t id = node->mount_id ? node->mount_id : node->dev_id;
  for (struct mount_entry *m = ns->stacked; m; m = m->next) {
    if (m->target[1] || m->info.mount_id != id) { continue; }
    if (m->fs) { return node->backend == VFS_RAMFS && node->ramfs.fs == m->fs && node->ramfs.index == m->root; }
    return node->backend == VFS_EXT2 && node->ino == 2;
  }
  return false;
}

int mountns_unstack(bool detach) {
  struct domain *d = cell_current_domain_internal();
  struct mount_namespace *ns = view(d);
  if (!ns->stacked) { return -22; }
  if (!detach) { return -16; }
  struct mount_entry *root = ns->stacked;
  while (root && root->target[1]) { root = root->next; }
  if (!root || root->locked) { return -1; }
  if (d->cwd[1] || (d->cwd_covered_mount && d->cwd_covered_mount != root->info.mount_id)) { return -22; }
  struct mount_entry *old = ns->stacked;
  ns->stacked = NULL;
  drop_entries(old);
  return 0;
}
