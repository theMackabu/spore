#include "proc/capabilities.h"
#include "proc/domain.h"
#include "proc/thread.h"
#include "proc/user_namespace.h"

static const uint32_t supported =
  (1u << CAP_CHOWN) | (1u << CAP_DAC_OVERRIDE) | (1u << CAP_DAC_READ_SEARCH) | (1u << CAP_FOWNER) | (1u << CAP_FSETID) |
  (1u << CAP_KILL) | (1u << CAP_SETGID) | (1u << CAP_SETUID) | (1u << CAP_SETPCAP) | (1u << CAP_NET_ADMIN) |
  (1u << CAP_SYS_CHROOT) | (1u << CAP_SYS_ADMIN) | (1u << CAP_SYS_BOOT) | (1u << CAP_SYS_RESOURCE);

struct linux_caps cell_initial_caps(void) {
  return (struct linux_caps){.effective = supported, .permitted = supported, .bounding = supported};
}

bool cell_ns_capable(const struct user_namespace *owner, enum linux_capability cap) {
  struct thread *t = cell_current_thread_internal();
  return t && userns_contains(t->domain->user_ns, owner) && (t->capabilities.effective & (1u << cap));
}

bool cell_capable(enum linux_capability cap) {
  return cell_ns_capable(NULL, cap);
}

int cell_cap_bound(bool drop, unsigned cap) {
  if (cap > 40) { return -22; }
  struct linux_caps *c = &cell_current_thread_internal()->capabilities;
  uint32_t bit = cap < 32 ? 1u << cap : 0;
  if (!drop) { return !!(c->bounding & bit); }
  if (!cell_ns_capable(cell_current_domain_internal()->user_ns, CAP_SETPCAP)) { return -1; }
  c->bounding &= ~bit;
  return 0;
}

int cell_cap_set(uint32_t effective, uint32_t permitted, uint32_t inheritable) {
  struct linux_caps *c = &cell_current_thread_internal()->capabilities;
  uint32_t added = inheritable & ~c->inheritable;
  if ((effective & ~permitted) || (permitted & ~c->permitted) || (added & ~c->bounding) ||
      ((added & ~c->permitted) && !cell_ns_capable(cell_current_domain_internal()->user_ns, CAP_SETPCAP))) {
    return -1;
  }
  c->effective = effective;
  c->permitted = permitted;
  c->inheritable = inheritable;
  return 0;
}

void cell_caps_after_setuid(struct domain *d, uint32_t old_euid, bool had_root) {
  bool lost_root = had_root && d->uid && d->euid && d->saved_uid;
  if (!lost_root && old_euid == d->euid) { return; }
  for (size_t i = 0; i < cell_thread_slot_limit(); ++i) {
    struct thread *t = cell_thread_slot(i);
    if (!t || t->domain != d) { continue; }
    struct linux_caps *c = &t->capabilities;
    if (lost_root) {
      c->effective = c->permitted = 0;
    } else if (!old_euid && d->euid) {
      c->effective = 0;
    } else if (old_euid && !d->euid) {
      c->effective = c->permitted;
    }
  }
}

void cell_caps_after_exec(const struct domain *d) {
  struct thread *t = cell_current_thread_internal();
  struct linux_caps *c = &t->capabilities;
  uint32_t euid = userns_visible(d->user_ns, false, d->euid);
  uint32_t permitted = (!userns_visible(d->user_ns, false, d->uid) || !euid) ? c->bounding | c->inheritable : 0;
  if (t->no_new_privs) { permitted &= c->permitted; }
  c->permitted = permitted;
  c->effective = euid ? 0 : permitted;
}
