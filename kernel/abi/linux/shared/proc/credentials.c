#include "../usercopy.h"
#include "handlers.h"
#include "proc/domain.h"
#include "lib/mem.h"

int64_t sys_getres_ids(bool group, uint64_t real, uint64_t effective, uint64_t saved) {
  struct domain *d = cell_current_domain_internal();
  if (d == NULL) { return -3; }
  uint32_t ids[] = {group ? d->gid : d->uid, group ? d->egid : d->euid, group ? d->saved_gid : d->saved_uid};
  uint64_t addresses[] = {real, effective, saved};
  for (unsigned i = 0; i < 3; ++i) {
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
  if (count && bytes && (!syscall_user_writable(list, bytes) ||
      !vmm_copy_to_user(syscall_active_as(), list, d->groups, bytes))) { return -14; }
  return d->group_count;
}

int64_t sys_setgroups(uint64_t count, uint64_t list) {
  struct domain *d = cell_current_domain_internal();
  if (d == NULL || d->euid != 0) { return -1; }
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
