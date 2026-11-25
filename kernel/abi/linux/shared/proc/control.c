#include "abi/linux/shared/usercopy.h"
#include "handlers.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/pid_namespace.h"
#include "proc/seccomp.h"
#include "proc/thread.h"

enum {
  PR_SET_PDEATHSIG = 1,
  PR_GET_PDEATHSIG = 2,
  PR_GET_DUMPABLE = 3,
  PR_SET_DUMPABLE = 4,
  PR_SET_NAME = 15,
  PR_GET_NAME = 16,
  PR_CAPBSET_READ = 23,
  PR_CAPBSET_DROP = 24,
  PR_GET_SECCOMP = 21,
  PR_SET_SECCOMP = 22,
  PR_SET_NO_NEW_PRIVS = 38,
  PR_GET_NO_NEW_PRIVS = 39,
  PR_SET_VMA = 0x53564d41,
  PR_SET_VMA_ANON_NAME = 0,
  ESRCH = 3,
  EFAULT = 14,
  EINVAL = 22,
};

int64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
  switch (option) {
  case PR_SET_PDEATHSIG:
    return cell_set_parent_death_signal(cell_current_thread_internal(), arg2);
  case PR_GET_PDEATHSIG: {
    int32_t signal = cell_current_thread_internal()->parent_death_signal;
    return syscall_user_writable(arg2, sizeof(signal)) &&
               vmm_copy_to_user(syscall_active_as(), arg2, &signal, sizeof(signal))
             ? 0
             : -EFAULT;
  }
  case PR_CAPBSET_READ:
  case PR_CAPBSET_DROP:
    return arg2 > 40 || arg3 || arg4 || arg5 ? -EINVAL : cell_cap_bound(option == PR_CAPBSET_DROP, arg2);
  case PR_GET_SECCOMP:
    return cell_current_domain_internal()->filter ? 2 : 0;
  case PR_SET_SECCOMP:
    return arg2 == 2 ? sys_seccomp(1, 0, arg3) : -EINVAL;
  case PR_GET_DUMPABLE:
    return 1;
  case PR_SET_DUMPABLE:
    return arg2 <= 1 && arg3 == 0 && arg4 == 0 && arg5 == 0 ? 0 : -(int64_t)EINVAL;
  case PR_SET_NAME: {
    char name[16];
    if (arg2 == 0) { return -(int64_t)EFAULT; }
    for (size_t i = 0; i < sizeof(name); ++i) {
      if (!syscall_user_readable(arg2 + i, 1) || !vmm_copy_from_user(syscall_active_as(), &name[i], arg2 + i, 1)) {
        return -(int64_t)EFAULT;
      }
      if (name[i] == '\0') {
        cell_set_current_name(name);
        return 0;
      }
    }
    name[sizeof(name) - 1] = '\0';
    cell_set_current_name(name);
    return 0;
  }
  case PR_GET_NAME: {
    char name[16];
    const char *current = cell_current_name();
    size_t i = 0;
    for (; i + 1 < sizeof(name) && current[i] != '\0'; ++i) {
      name[i] = current[i];
    }
    name[i] = '\0';
    return syscall_user_writable(arg2, sizeof(name)) && vmm_copy_to_user(syscall_active_as(), arg2, name, sizeof(name))
             ? 0
             : -(int64_t)EFAULT;
  }
  case PR_SET_NO_NEW_PRIVS:
    if (arg2 != 1 || arg3 || arg4 || arg5) { return -EINVAL; }
    cell_current_thread_internal()->no_new_privs = true;
    return 0;
  case PR_GET_NO_NEW_PRIVS:
    if (arg2 || arg3 || arg4 || arg5) { return -EINVAL; }
    return cell_current_thread_internal()->no_new_privs;
  case PR_SET_VMA:
    return arg2 == PR_SET_VMA_ANON_NAME ? 0 : -(int64_t)EINVAL;
  default:
    kprintf("[kernel] prctl option=%d unsupported pid=%d name=%s\n", (int)option, cell_current_pid(),
            cell_current_name());
    return -(int64_t)EINVAL;
  }
}

int64_t sys_sched_getscheduler(int tid) {
  if (tid < 0) { return -EINVAL; }
  if (tid && !cell_find_thread(pidns_global(pidns_current(), tid))) { return -ESRCH; }
  return 0;
}

int64_t sys_sched_getparam(int tid, uint64_t param) {
  if (!param) { return -EINVAL; }
  int64_t policy = sys_sched_getscheduler(tid);
  if (policy < 0) { return policy; }
  int32_t priority = 0;
  return syscall_user_writable(param, sizeof(priority)) &&
             vmm_copy_to_user(syscall_active_as(), param, &priority, sizeof(priority))
           ? 0
           : -EFAULT;
}
