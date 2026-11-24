#include "proc/info.h"
#include "abi/linux/shared/usercopy.h"
#include "abi/linux/shared/layout.h"
#include "abi/linux/shared/usage.h"
#include "handlers.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/thread.h"

enum { EINVAL = 22, EFAULT = 14, RUSAGE_CHILDREN = -1, RUSAGE_SELF = 0, RUSAGE_THREAD = 1 };

int linux_write_rusage(struct domain *domain, uint64_t addr, const struct resource_usage *usage) {
  struct rusage64 out = {
    .ru_utime = {.tv_sec = usage->user_ticks / 100, .tv_usec = usage->user_ticks % 100 * 10000},
    .ru_maxrss = (int64_t)usage->peak_pages * (PAGE_SIZE / 1024),
    .ru_minflt = (int64_t)usage->minor_faults,
    .ru_majflt = (int64_t)usage->major_faults,
  };
  return cell_domain_ensure_user_range(domain, addr, sizeof(out), VMM_ACCESS_WRITE) &&
         vmm_copy_to_user(cell_domain_as(domain), addr, &out, sizeof(out)) ? 0 : -EFAULT;
}

int64_t sys_getrusage(int who, uint64_t addr) {
  struct thread *thread = cell_current_thread_internal();
  struct domain *domain = thread->domain;
  if (who == RUSAGE_CHILDREN) { return linux_write_rusage(domain, addr, &domain->children_usage); }
  if (who != RUSAGE_SELF && who != RUSAGE_THREAD) { return -EINVAL; }
  struct resource_usage usage = cell_process_usage(domain);
  if (who == RUSAGE_THREAD) {
    usage.user_ticks = thread->cpu_ticks;
    usage.minor_faults = thread->minor_faults;
    usage.major_faults = thread->major_faults;
  }
  return linux_write_rusage(domain, addr, &usage);
}

int64_t sys_spore_procinfo(uint64_t addr, uint64_t bytes) {
  size_t max = bytes / sizeof(struct proc_info);
  if (addr == 0 || max == 0) { return (int64_t)cell_proc_info(NULL, 0); }
  constexpr int PROCINFO_CHUNK = 64;
  static struct proc_info infos[PROCINFO_CHUNK];
  size_t request = max < PROCINFO_CHUNK ? max : PROCINFO_CHUNK;
  size_t count = cell_proc_info(infos, request);
  size_t copy = count < request ? count : request;
  return syscall_user_writable(addr, copy * sizeof(infos[0])) &&
             vmm_copy_to_user(syscall_active_as(), addr, infos, copy * sizeof(infos[0]))
           ? (int64_t)count
           : -(int64_t)EFAULT;
}
