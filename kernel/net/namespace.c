#include "proc/namespace.h"
#include "net/namespace.h"
#include "proc/domain.h"
#include "proc/thread.h"
#include "proc/user_namespace.h"
#include "mm/pool.h"

static struct object_pool namespaces;

int netns_create(struct thread *creator, struct net_namespace **out) {
  if (!(creator->capabilities.effective & (1u << CAP_SYS_ADMIN))) { return -1; }
  if (creator->domain->refcount != 1) { return -95; }
  if (!namespaces.limit && !pool_init(&namespaces, sizeof(struct net_namespace), MAX_DOMAINS)) { return -12; }
  struct net_namespace *ns = pool_alloc(&namespaces);
  if (!ns) { return -12; }
  ns->ino = namespace_new_ino();
  ns->refs = 1;
  ns->owner = userns_retain(creator->domain->user_ns);
  *out = ns;
  return 0;
}

struct net_namespace *netns_retain(struct net_namespace *ns) {
  if (ns) { ++ns->refs; }
  return ns;
}

void netns_release(struct net_namespace *ns) {
  if (ns && !--ns->refs) {
    userns_release(ns->owner);
    pool_free(&namespaces, ns);
  }
}
