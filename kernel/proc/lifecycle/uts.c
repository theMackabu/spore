#include "proc/uts.h"
#include "proc/thread.h"
#include "proc/capabilities.h"
#include "proc/user_namespace.h"
#include "mm/pool.h"
#include "proc/domain.h"

struct uts_namespace {
  unsigned refs;
  struct user_namespace *owner;
  char hostname[UTS_NAME_SIZE];
  char domainname[UTS_NAME_SIZE];
};

static struct object_pool namespaces;
static struct uts_namespace initial = {.hostname = "spore.local", .domainname = "Spore"};

bool uts_init(size_t domains) {
  return pool_init(&namespaces, sizeof(struct uts_namespace), domains + 1);
}

struct uts_namespace *uts_retain(struct uts_namespace *ns) {
  if (ns) { ++ns->refs; }
  return ns;
}

void uts_release(struct uts_namespace *ns) {
  if (ns && --ns->refs == 0) { userns_release(ns->owner); pool_free(&namespaces, ns); }
}

int uts_unshare(struct thread *leader) {
  struct domain *domain = leader->domain;
  if (!(leader->capabilities.effective & (1u << CAP_SYS_ADMIN))) { return -1; }
  if (domain->refcount != 1) { return -95; }
  struct uts_namespace *copy = pool_alloc(&namespaces);
  if (!copy) { return -12; }
  *copy = domain->uts ? *domain->uts : initial;
  copy->refs = 1;
  copy->owner = userns_retain(domain->user_ns);
  uts_release(domain->uts);
  domain->uts = copy;
  return 0;
}

char *uts_name(struct domain *domain, bool hostname) {
  struct uts_namespace *ns = domain->uts ? domain->uts : &initial;
  return hostname ? ns->hostname : ns->domainname;
}

bool uts_may_set(const struct domain *domain) {
  return cell_ns_capable(domain->uts ? domain->uts->owner : NULL, CAP_SYS_ADMIN);
}
