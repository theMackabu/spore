#include "proc/pid_namespace.h"
#include "proc/namespace.h"
#include "proc/domain.h"
#include "proc/thread.h"
#include "proc/user_namespace.h"
#include "mm/pool.h"

struct pid_namespace {
  uint64_t ino;
  unsigned refs, depth;
  int init, last;
  bool dead;
  struct user_namespace *owner;
  struct pid_namespace *parent;
  struct pid_entry *ids;
};

struct pid_entry {
  unsigned refs;
  int local, global;
  struct pid_namespace *ns;
  struct pid_entry *ancestor, *next, **link;
};

static struct object_pool namespaces, entries;

struct pid_namespace *pidns_active(const struct domain *d) { return d && d->pid_entry ? d->pid_entry->ns : NULL; }
struct pid_namespace *pidns_current(void) { return pidns_active(cell_current_domain_internal()); }
uint64_t pidns_ino(const struct pid_namespace *ns) { return ns->ino; }

struct pid_namespace *pidns_retain(struct pid_namespace *ns) {
  if (ns) { ++ns->refs; }
  return ns;
}

void pidns_release(struct pid_namespace *ns) {
  while (ns && !--ns->refs) {
    struct pid_namespace *parent = ns->parent;
    userns_release(ns->owner);
    pool_free(&namespaces, ns);
    ns = parent;
  }
}

int pidns_create(struct thread *thread, struct pid_namespace **out) {
  struct domain *d = thread->domain;
  struct pid_namespace *parent = d->child_pid_ns;
  if (!(thread->capabilities.effective & (1u << CAP_SYS_ADMIN))) { return -1; }
  if (d->refcount != 1) { return -22; }
  if (parent && parent->depth == 32) { return -28; }
  if (!namespaces.limit && !pool_init(&namespaces, sizeof(struct pid_namespace), MAX_DOMAINS)) { return -12; }
  struct pid_namespace *ns = pool_alloc(&namespaces);
  if (!ns) { return -12; }
  ns->ino = namespace_new_ino();
  ns->refs = 1;
  ns->owner = userns_retain(d->user_ns);
  ns->parent = pidns_retain(parent);
  ns->depth = parent ? parent->depth + 1 : 1;
  *out = ns;
  return 0;
}

int pidns_local(const struct pid_namespace *ns, int global) {
  if (!ns || !global) { return global; }
  for (struct pid_entry *id = ns->ids; id; id = id->next) {
    if (id->global == global) { return id->local; }
  }
  return 0;
}

int pidns_global(const struct pid_namespace *ns, int local) {
  if (!ns || !local) { return local; }
  for (struct pid_entry *id = ns->ids; id; id = id->next) {
    if (id->local == local) { return id->global; }
  }
  return 0;
}

int pidns_self_view(int global) { return pidns_local(pidns_current(), global); }

int pidns_last(const struct pid_namespace *ns) { return ns ? ns->last : (int)cell_last_task_id(); }

bool pidns_init_process(const struct domain *d) {
  return d && d->pid_entry && d->pid_entry->local == 1;
}

struct pid_entry *pidns_retain_id(struct pid_entry *id) {
  if (id) { ++id->refs; }
  return id;
}

void pidns_drop_id(struct pid_entry *id) {
  while (id && !--id->refs) {
    struct pid_entry *ancestor = id->ancestor;
    *id->link = id->next;
    if (id->next) { id->next->link = id->link; }
    if (id->global == id->ns->init && !id->ns->dead) { id->ns->init = 0; }
    pidns_release(id->ns);
    pool_free(&entries, id);
    id = ancestor;
  }
}

struct pid_entry *pidns_hold_id(struct pid_namespace *ns, int global) {
  for (struct pid_entry *id = ns ? ns->ids : NULL; id; id = id->next) {
    if (id->global == global) { ++id->refs; return id; }
  }
  return NULL;
}

int pidns_allocate(struct pid_namespace *ns, int global, struct pid_entry **out) {
  struct pid_entry *head = NULL, **slot = &head;
  for (; ns; ns = ns->parent) {
    if (ns->dead) { pidns_drop_id(head); return -12; }
    if (!entries.limit && !pool_init(&entries, sizeof(struct pid_entry), SIZE_MAX)) { return -12; }
    struct pid_entry *id = pool_alloc(&entries);
    if (!id) { pidns_drop_id(head); return -12; }
    int local = 1;
    if (!ns->init) { ns->init = global; }
    else {
      local = ns->last;
      do { local = local == INT32_MAX ? 2 : local + 1; } while (pidns_global(ns, local));
    }
    ns->last = local;
    *id = (struct pid_entry){.refs = 1, .global = global, .local = local, .ns = pidns_retain(ns),
                             .link = &ns->ids, .next = ns->ids};
    if (id->next) { id->next->link = &id->next; }
    ns->ids = id;
    *slot = id;
    slot = &id->ancestor;
  }
  *out = head;
  return 0;
}

int pidns_reaper(const struct domain *domain) {
  struct pid_namespace *ns = pidns_active(domain);
  while (ns && ns->dead) { ns = ns->parent; }
  return ns ? ns->init : 1;
}

bool pidns_mark_dead(struct domain *domain) {
  struct pid_namespace *ns = pidns_active(domain);
  if (!ns || ns->dead || ns->init != domain->id) { return false; }
  ns->dead = true;
  return true;
}

bool pidns_has_dead_parent(const struct domain *domain) {
  for (struct pid_namespace *ns = pidns_active(domain); ns; ns = ns->parent) {
    if (ns->dead) { return true; }
  }
  return false;
}
