#include "proc/user_namespace.h"
#include "fs/mount_namespace.h"
#include "lib/kstr.h"
#include "mm/pool.h"
#include "net/namespace.h"
#include "proc/domain.h"
#include "proc/ipc_namespace.h"
#include "proc/namespace.h"
#include "proc/pid_namespace.h"
#include "proc/thread.h"
#include "proc/uts.h"

static struct object_pool namespaces;

bool userns_init(size_t capacity) {
  return pool_init(&namespaces, sizeof(struct user_namespace), capacity + 1);
}

struct user_namespace *userns_retain(struct user_namespace *ns) {
  if (ns) { ++ns->refs; }
  return ns;
}

void userns_release(struct user_namespace *ns) {
  while (ns && !--ns->refs) {
    struct user_namespace *parent = ns->parent;
    pool_free(&namespaces, ns);
    ns = parent;
  }
}

bool userns_contains(const struct user_namespace *ancestor, const struct user_namespace *child) {
  while (child && child != ancestor) {
    child = child->parent;
  }
  return child == ancestor;
}

static int create_user_namespace(struct thread *leader) {
  struct domain *d = leader->domain;
  if (d->refcount != 1) { return -95; }
  struct user_namespace *parent = d->user_ns;
  if (parent && parent->depth >= 32) { return -28; }
  if (parent &&
      (!parent->uid.mapped || parent->uid.outer != d->euid || !parent->gid.mapped || parent->gid.outer != d->egid)) {
    return -1;
  }
  if (!leader->no_new_privs || !d->euid || !d->egid || d->uid != d->euid || d->saved_uid != d->euid ||
      d->gid != d->egid || d->saved_gid != d->egid || !str_eq(d->chroot, "/")) {
    return -1;
  }
  struct user_namespace *ns = pool_alloc(&namespaces);
  if (!ns) { return -12; }
  ns->ino = namespace_new_ino();
  ns->refs = 1;
  ns->parent = parent;
  ns->depth = parent ? parent->depth + 1 : 1;
  ns->groups_denied = parent && parent->groups_denied;
  ns->uid.outer = d->euid;
  ns->gid.outer = d->egid;
  d->user_ns = ns;
  leader->capabilities = cell_initial_caps();
  return 0;
}

uint32_t userns_visible(const struct user_namespace *ns, bool group, uint32_t outer) {
  if (!ns) { return outer; }
  const struct id_map *map = group ? &ns->gid : &ns->uid;
  return map->mapped && map->outer == outer ? map->inner : USER_OVERFLOW_ID;
}

bool userns_translate(const struct user_namespace *ns, bool group, uint32_t inner, uint32_t *outer) {
  if (!ns || inner == UINT32_MAX) {
    *outer = inner;
    return true;
  }
  const struct id_map *map = group ? &ns->gid : &ns->uid;
  if (!map->mapped || map->inner != inner) { return false; }
  *outer = map->outer;
  return true;
}

static bool number(const char **text, uint32_t *value) {
  const char *p = *text;
  while (*p == ' ' || *p == '\t') {
    ++p;
  }
  if (*p < '0' || *p > '9') { return false; }
  uint32_t n = 0;
  do {
    unsigned digit = *p++ - '0';
    if (n > (UINT32_MAX - digit) / 10) { return false; }
    n = n * 10 + digit;
  } while (*p >= '0' && *p <= '9');
  *value = n;
  *text = p;
  return true;
}

int userns_write_map(struct domain *actor, struct user_namespace *ns, bool group, bool setgroups, const char *text) {
  if (!ns || (actor->user_ns != ns && actor->user_ns != ns->parent) || actor->euid != ns->uid.outer) { return -1; }
  if (actor->user_ns == ns && !cell_ns_capable(ns, (group || setgroups) ? CAP_SETGID : CAP_SETUID)) { return -1; }
  if (setgroups) {
    if (!str_eq(text, "deny") && !str_eq(text, "deny\n")) { return -22; }
    if (ns->gid.mapped && !ns->groups_denied) { return -1; }
    ns->groups_denied = true;
    return 0;
  }
  struct id_map *map = group ? &ns->gid : &ns->uid;
  if (map->mapped || (group && !ns->groups_denied)) { return -1; }
  uint32_t inner, outer, count;
  if (!number(&text, &inner) || !number(&text, &outer) || !number(&text, &count) || inner == UINT32_MAX || count != 1 ||
      (*text && !str_eq(text, "\n"))) {
    return -22;
  }
  if (outer == UINT32_MAX || !userns_translate(ns->parent, group, outer, &outer)) { return -1; }
  if (outer != map->outer || (group && actor->egid != outer)) { return -1; }
  map->inner = inner;
  map->mapped = true;
  return 0;
}

int cell_unshare_namespaces(struct thread *thread, uint64_t flags) {
  if (flags & ~(CLONE_FS | CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWPID)) {
    return -22;
  }
  struct domain *d = thread->domain;
  if ((flags & CLONE_FS) && d->refcount != 1) { return -38; }
  if (!(flags & ~CLONE_FS)) { return 0; }
  struct linux_caps old_caps = thread->capabilities;
  struct uts_namespace *old_uts = uts_retain(d->uts);
  int rc = 0;
  if (flags & CLONE_NEWUSER) {
    rc = create_user_namespace(thread);
    if (rc) {
      uts_release(old_uts);
      return rc;
    }
  }
  struct pid_namespace *pid = NULL;
  struct net_namespace *net = NULL;
  struct ipc_namespace *ipc = NULL;
  if (flags & CLONE_NEWPID) { rc = pidns_create(thread, &pid); }
  if (!rc && (flags & CLONE_NEWIPC)) { rc = ipcns_create(thread, &ipc); }
  if (!rc && (flags & CLONE_NEWNET)) { rc = netns_create(thread, &net); }
  if (!rc && (flags & CLONE_NEWUTS)) { rc = uts_unshare(thread); }
  if (!rc && (flags & CLONE_NEWNS)) { rc = mountns_unshare(thread); }
  if (rc) {
    pidns_release(pid);
    netns_release(net);
    ipcns_release(ipc);
    uts_release(d->uts);
    d->uts = old_uts;
    if (flags & CLONE_NEWUSER) {
      struct user_namespace *parent = userns_retain(d->user_ns->parent);
      userns_release(d->user_ns);
      d->user_ns = parent;
      thread->capabilities = old_caps;
    }
  } else {
    uts_release(old_uts);
    if (pid) {
      pidns_release(d->child_pid_ns);
      d->child_pid_ns = pid;
    }
    if (net) {
      netns_release(d->net_ns);
      d->net_ns = net;
    }
    if (ipc) {
      ipcns_release(d->ipc_ns);
      d->ipc_ns = ipc;
    }
  }
  return rc;
}
