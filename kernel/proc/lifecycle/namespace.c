#include "proc/namespace.h"
#include "proc/domain.h"
#include "proc/user_namespace.h"
#include "proc/ipc_namespace.h"
#include "net/namespace.h"
#include "proc/uts.h"
#include "fs/mount_namespace.h"

uint64_t namespace_new_ino(void) {
  static uint64_t last = NS_COUNT;
  return ++last;
}

struct namespace_ref namespace_of(const struct domain *d, enum namespace_kind kind) {
  void *object = NULL;
  switch (kind) {
  case NS_MOUNT: object = d->mount_ns; break;
  case NS_USER: object = d->user_ns; break;
  case NS_NET: object = d->net_ns; break;
  case NS_IPC: object = d->ipc_ns; break;
  case NS_UTS: object = d->uts; break;
  default: break;
  }
  return (struct namespace_ref){kind, object};
}

const char *namespace_name(enum namespace_kind kind) {
  static const char *const names[NS_COUNT] = {[NS_MOUNT] = "mnt", [NS_USER] = "user", [NS_UTS] = "uts", [NS_IPC] = "ipc", [NS_NET] = "net"};
  return names[kind];
}

uint64_t namespace_ino(struct namespace_ref ns) {
  if (!ns.object) { return 1 + ns.kind; }
  switch (ns.kind) {
  case NS_MOUNT: return mountns_ino(ns.object);
  case NS_USER: return ((struct user_namespace *)ns.object)->ino;
  case NS_NET: return ((struct net_namespace *)ns.object)->ino;
  case NS_IPC: return ((struct ipc_namespace *)ns.object)->ino;
  case NS_UTS: return uts_ino(ns.object);
  default: return 0;
  }
}

void namespace_retain(struct namespace_ref ns) {
  switch (ns.kind) {
  case NS_MOUNT: mountns_retain(ns.object); break;
  case NS_USER: userns_retain(ns.object); break;
  case NS_NET: netns_retain(ns.object); break;
  case NS_IPC: ipcns_retain(ns.object); break;
  case NS_UTS: uts_retain(ns.object); break;
  default: break;
  }
}

void namespace_release(struct namespace_ref ns) {
  switch (ns.kind) {
  case NS_MOUNT: mountns_release(ns.object); break;
  case NS_USER: userns_release(ns.object); break;
  case NS_NET: netns_release(ns.object); break;
  case NS_IPC: ipcns_release(ns.object); break;
  case NS_UTS: uts_release(ns.object); break;
  default: break;
  }
}
