#include "proc/user_namespace.h"
#include "proc/domain.h"
#include "proc/thread.h"
#include "proc/uts.h"
#include "mm/pool.h"
#include "lib/kstr.h"

static struct object_pool namespaces;

bool userns_init(size_t capacity) {
  return pool_init(&namespaces, sizeof(struct user_namespace), capacity + 1);
}

struct user_namespace *userns_retain(struct user_namespace *ns) {
  if (ns) { ++ns->refs; }
  return ns;
}

void userns_release(struct user_namespace *ns) {
  if (ns && --ns->refs == 0) { pool_free(&namespaces, ns); }
}

static int create_user_namespace(struct thread *leader) {
  struct domain *d = leader->domain;
  if (d->user_ns || d->refcount != 1) { return -95; }
  if (!leader->no_new_privs || !d->euid || !d->egid || d->uid != d->euid || d->saved_uid != d->euid ||
      d->gid != d->egid || d->saved_gid != d->egid || !str_eq(d->chroot, "/")) { return -1; }
  struct user_namespace *ns = pool_alloc(&namespaces);
  if (!ns) { return -12; }
  ns->refs = 1;
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
  if (!ns || inner == UINT32_MAX) { *outer = inner; return true; }
  const struct id_map *map = group ? &ns->gid : &ns->uid;
  if (!map->mapped || map->inner != inner) { return false; }
  *outer = map->outer;
  return true;
}

static bool number(const char **text, uint32_t *value) {
  const char *p = *text;
  while (*p == ' ' || *p == '\t') { ++p; }
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
  if (!ns || (actor->user_ns && actor->user_ns != ns) || actor->euid != ns->uid.outer) { return -1; }
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
  if (!number(&text, &inner) || !number(&text, &outer) || !number(&text, &count) ||
      inner == UINT32_MAX || count != 1 || (*text && !str_eq(text, "\n"))) { return -22; }
  if (outer != map->outer || (group && actor->egid != outer)) { return -1; }
  map->inner = inner;
  map->mapped = true;
  return 0;
}

int cell_unshare_namespaces(struct thread *thread, uint64_t flags) {
  if (flags & ~(CLONE_NEWUSER | (uint64_t)CLONE_NEWUTS)) { return -22; }
  if (!(flags & CLONE_NEWUSER)) { return flags ? uts_unshare(thread) : 0; }
  struct linux_caps old_caps = thread->capabilities;
  int rc = create_user_namespace(thread);
  if (rc || !(flags & CLONE_NEWUTS)) { return rc; }
  rc = uts_unshare(thread);
  if (rc) {
    userns_release(thread->domain->user_ns);
    thread->domain->user_ns = NULL;
    thread->capabilities = old_caps;
  }
  return rc;
}
