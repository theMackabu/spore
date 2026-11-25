#include "proc/pid_namespace.h"
#include "../usercopy.h"
#include "handlers.h"
#include "proc/domain.h"
#include "proc/thread.h"
#include "proc/user_namespace.h"
#include "lib/mem.h"

int64_t sys_set_ids(bool group, bool single, uint32_t real, uint32_t effective, uint32_t saved) {
  struct user_namespace *ns = cell_current_domain_internal()->user_ns;
  if (!userns_translate(ns, group, real, &real) || !userns_translate(ns, group, effective, &effective) ||
      !userns_translate(ns, group, saved, &saved)) { return -22; }
  if (single) { return group ? cell_setgid_current(real) : cell_setuid_current(real); }
  return cell_setres_ids(group, real, effective, saved);
}

int64_t sys_getres_ids(bool group, uint64_t real, uint64_t effective, uint64_t saved) {
  struct domain *d = cell_current_domain_internal();
  if (d == NULL) { return -3; }
  uint32_t ids[] = {group ? d->gid : d->uid, group ? d->egid : d->euid, group ? d->saved_gid : d->saved_uid};
  uint64_t addresses[] = {real, effective, saved};
  for (unsigned i = 0; i < 3; ++i) {
    ids[i] = userns_visible(d->user_ns, group, ids[i]);
    if (!syscall_user_writable(addresses[i], sizeof(ids[i])) ||
        !vmm_copy_to_user(syscall_active_as(), addresses[i], &ids[i], sizeof(ids[i]))) { return -14; }
  }
  return 0;
}

int64_t sys_getgroups(int count, uint64_t list) {
  struct domain *d = cell_current_domain_internal();
  if (d == NULL) { return -3; }
  if (count < 0 || (count && (unsigned)count < d->group_count)) { return -22; }
  size_t bytes = d->group_count * sizeof(d->groups[0]);
  if (count && bytes) {
    uint32_t groups[DOMAIN_GROUP_LIMIT];
    for (unsigned i = 0; i < d->group_count; ++i) { groups[i] = userns_visible(d->user_ns, true, d->groups[i]); }
    if (!syscall_user_writable(list, bytes) || !vmm_copy_to_user(syscall_active_as(), list, groups, bytes)) { return -14; }
  }
  return d->group_count;
}

int64_t sys_setgroups(uint64_t count, uint64_t list) {
  struct domain *d = cell_current_domain_internal();
  if (d == NULL || !cell_capable(CAP_SETGID)) { return -1; }
  if (count > DOMAIN_GROUP_LIMIT) { return -22; }
  uint32_t groups[DOMAIN_GROUP_LIMIT];
  size_t bytes = count * sizeof(groups[0]);
  if (bytes && !vmm_copy_from_user(syscall_active_as(), groups, list, bytes)) { return -14; }
  for (size_t i = 0; i < count; ++i) {
    if (groups[i] == UINT32_MAX) { return -22; }
  }
  kmemcpy(d->groups, groups, bytes);
  d->group_count = (unsigned)count;
  return 0;
}

int64_t sys_capabilities(bool set, uint64_t header, uint64_t data) {
  constexpr uint32_t version1 = 0x19980330, version2 = 0x20071026, version3 = 0x20080522;
  struct { uint32_t version; int32_t pid; } hdr;
  struct { uint32_t effective, permitted, inheritable; } caps[2] = {};
  struct user_address_space *as = syscall_active_as();
  if (!syscall_user_readable(header, sizeof(hdr)) || !vmm_copy_from_user(as, &hdr, header, sizeof(hdr))) { return -14; }
  if (hdr.version != version1 && hdr.version != version2 && hdr.version != version3) {
    if (!syscall_user_writable(header, sizeof(version3)) ||
        !vmm_copy_to_user(as, header, &version3, sizeof(version3))) { return -14; }
    return !set && !data ? 0 : -22;
  }
  if (!set && !data) { return 0; }
  if (hdr.pid < 0) { return -22; }
  struct thread *self = cell_current_thread_internal();
  if (set && hdr.pid && hdr.pid != pidns_self_view(self->tid)) { return -1; }
  struct thread *task = hdr.pid ? cell_find_thread(pidns_global(pidns_current(), hdr.pid)) : self;
  if (!task || !task->domain) { return -3; }
  size_t bytes = hdr.version == version1 ? sizeof(caps[0]) : sizeof(caps);
  if (!set) {
    caps[0].effective = task->capabilities.effective;
    caps[0].permitted = task->capabilities.permitted;
    caps[0].inheritable = task->capabilities.inheritable;
    return syscall_user_writable(data, bytes) && vmm_copy_to_user(as, data, caps, bytes) ? 0 : -14;
  }
  if (!syscall_user_readable(data, bytes) || !vmm_copy_from_user(as, caps, data, bytes)) { return -14; }
  if (caps[1].effective || caps[1].permitted || caps[1].inheritable) { return -1; }
  return cell_cap_set(caps[0].effective, caps[0].permitted, caps[0].inheritable);
}
