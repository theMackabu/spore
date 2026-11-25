#include "proc/pid_namespace.h"
#include "../usercopy.h"
#include "handlers.h"
#include "exec/stack.h"
#include "proc/domain.h"
#include "proc/capabilities.h"

enum { EPERM = 1, EFAULT = 14, EINVAL = 22 };

struct rlimit64 {
  uint64_t rlim_cur;
  uint64_t rlim_max;
};

int64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_limit, uint64_t old_limit) {
  enum {
    RLIMIT_FSIZE=1, RLIMIT_STACK = 3,
    RLIMIT_NOFILE = 7,
    RLIMIT_AS = 9,
  };
  if (pid != 0 && (int)pid != pidns_self_view(cell_current_pid())) { return -(int64_t)EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  struct rlimit64 next;
  if (new_limit) {
    if (!syscall_user_readable(new_limit, sizeof(next)) ||
        !vmm_copy_from_user(syscall_active_as(), &next, new_limit, sizeof(next))) { return -EFAULT; }
    if (resource==RLIMIT_FSIZE) {
      if (next.rlim_cur>next.rlim_max) {return -EINVAL;}
      if (next.rlim_max>domain->fsize_hard && !cell_capable(CAP_SYS_RESOURCE)) {return -EPERM;}
    }
    if (resource == RLIMIT_NOFILE) {
      if (next.rlim_cur > next.rlim_max) { return -EINVAL; }
      if (next.rlim_max > MAX_FDS || (next.rlim_max > domain->nofile_hard && !cell_capable(CAP_SYS_RESOURCE))) { return -EPERM; }
    }
  }
  if (old_limit) {
    struct rlimit64 out = {UINT64_MAX, UINT64_MAX};
    if (resource==RLIMIT_FSIZE) {out=(struct rlimit64){domain->fsize_soft,domain->fsize_hard};}
    else if (resource == RLIMIT_STACK) { out = (struct rlimit64){USER_STACK_SIZE, USER_STACK_SIZE}; }
    else if (resource == RLIMIT_NOFILE) { out = (struct rlimit64){domain->nofile_soft, domain->nofile_hard}; }
    else if (resource == RLIMIT_AS) { out = (struct rlimit64){USER_STACK_TOP, USER_STACK_TOP}; }
    if (!syscall_user_writable(old_limit, sizeof(out)) ||
        !vmm_copy_to_user(syscall_active_as(), old_limit, &out, sizeof(out))) { return -EFAULT; }
  }
  if (new_limit && resource==RLIMIT_FSIZE) {domain->fsize_soft=next.rlim_cur;domain->fsize_hard=next.rlim_max;}
  if (new_limit && resource == RLIMIT_NOFILE) {
    domain->nofile_soft = (uint32_t)next.rlim_cur;
    domain->nofile_hard = (uint32_t)next.rlim_max;
  }
  return 0;
}
