#include "fs/procfs.h"

#include "arch/smp.h"
#include "proc/accounting.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/user_namespace.h"
#include "proc/info.h"
#include "arch/cpuinfo.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "fs/procfs_format.h"
#include "text.h"
#include "proc/thread.h"

#include <stddef.h>

static const char *thread_state_text(enum thread_state state) {
  switch (state) {
  case THREAD_RUNNABLE:
    return "running";
  case THREAD_BLOCKED:
    return "blocked";
  case THREAD_ZOMBIE:
    return "zombie";
  case THREAD_UNUSED:
    return "unused";
  }
  return "unknown";
}

static const char *wait_reason_text(enum wait_reason reason) {
  switch (reason) {
  case WAIT_NONE:
    return "-";
  case WAIT_CHILD:
    return "child";
  case WAIT_STDIN:
    return "stdin";
  case WAIT_SOCKET:
    return "socket";
  case WAIT_THREAD:
    return "thread";
  case WAIT_FUTEX:
    return "futex";
  case WAIT_POLL:
    return "poll";
  case WAIT_SLEEP:
    return "sleep";
  case WAIT_VFORK:
    return "vfork";
  case WAIT_FIFO_OPEN:
    return "fifo-open";
  case WAIT_PIPE:
    return "pipe";
  case WAIT_EPOLL:
    return "epoll";
  case WAIT_INOTIFY:
    return "inotify";
  case WAIT_PTY:
    return "pty";
  case WAIT_EVENTFD:
    return "eventfd";
  case WAIT_FLOCK:
    return "flock";
  default:
    return "?";
  }
}

static const char *domain_state_text(const struct domain *domain) {
  enum thread_state state = cell_domain_state(domain);
  return state == THREAD_UNUSED ? "unknown" : thread_state_text(state);
}

static char proc_state_char(enum thread_state state) {
  switch (state) {
  case THREAD_RUNNABLE: return 'R';
  case THREAD_BLOCKED: return 'S';
  case THREAD_ZOMBIE: return 'Z';
  default: return '?';
  }
}

static const char *domain_wait_text(const struct domain *domain) {
  if (domain == NULL || domain->zombie) { return "-"; }
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain == domain && thread->state == THREAD_BLOCKED) {
      return wait_reason_text(thread->wait.reason);
    }
  }
  return "-";
}

static size_t procinfo_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len,
                  "pid ppid state wait vsz_pages rss_pages minflt majflt cpu_ticks age_ticks budget_remaining "
                  "budget_max unsupported_syscalls last_unsupported_syscall unsupported_ioctls last_unsupported_ioctl "
                  "name exec_path cwd cmdline\n");
  for (size_t i = 0, limit = cell_domain_slot_limit(); i < limit; ++i) {
    const struct domain *domain = cell_domain_slot(i);
    if (domain == NULL || !domain->used) { continue; }
    struct cell_memory_accounting mem = {0};
    (void)cell_memory_accounting(domain, &mem);
    proc_append_u64(dst, cap, &len, (uint32_t)domain->id);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, (uint32_t)domain->parent_id);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain_state_text(domain));
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain_wait_text(domain));
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, mem.virtual_pages);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, mem.resident_pages);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, mem.minor_faults);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, mem.major_faults);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->cpu_ticks);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, cell_uptime_ticks() - domain->start_ticks);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->budget.remaining_ticks);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->budget.max_ticks);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->unsupported_syscalls);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->last_unsupported_syscall);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->unsupported_ioctls);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->last_unsupported_ioctl);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain->name);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain->exec_path[0] == '\0' ? "-" : domain->exec_path);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain->cwd);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain->cmdline[0] == '\0' ? domain->name : domain->cmdline);
    proc_append_char(dst, cap, &len, '\n');
  }
  return len;
}

struct proc_task {
  const struct domain *domain;
  const struct thread *thread;
};

static size_t proc_pid_status_text(char *dst, size_t cap, const struct proc_task *task) {
  size_t len = 0;
  const struct domain *domain = task->domain;
  struct cell_memory_accounting mem = {0};
  (void)cell_memory_accounting(domain, &mem);
  proc_append_str(dst, cap, &len, "Name:\t");
  proc_append_str(dst, cap, &len, domain->name);
  proc_append_str(dst, cap, &len, "\nUmask:\t0");
  for (int shift = 6; shift >= 0; shift -= 3) {
    proc_append_char(dst, cap, &len, '0' + ((domain->umask >> shift) & 7));
  }
  proc_append_str(dst, cap, &len, "\nState:\t");
  enum thread_state state = task->thread ? task->thread->state : cell_domain_state(domain);
  proc_append_char(dst, cap, &len, proc_state_char(state));
  proc_append_str(dst, cap, &len, " (");
  proc_append_str(dst, cap, &len, thread_state_text(state));
  proc_append_str(dst, cap, &len, ")\nTgid:\t");
  proc_append_u64(dst, cap, &len, (uint32_t)domain->id);
  proc_append_str(dst, cap, &len, "\nPid:\t");
  proc_append_u64(dst, cap, &len, task->thread ? task->thread->tid : domain->id);
  proc_append_str(dst, cap, &len, "\nThreads:\t");
  proc_append_u64(dst, cap, &len, domain->refcount);
  proc_append_str(dst, cap, &len, "\nPPid:\t");
  proc_append_u64(dst, cap, &len, (uint32_t)domain->parent_id);
  struct user_namespace *view = cell_current_domain_internal()->user_ns;
  proc_append_str(dst, cap, &len, "\nUid:\t");
  proc_append_u64(dst, cap, &len, userns_visible(view, false, domain->uid));
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, userns_visible(view, false, domain->euid));
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, userns_visible(view, false, domain->saved_uid));
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, userns_visible(view, false, domain->euid));
  proc_append_str(dst, cap, &len, "\nGid:\t");
  proc_append_u64(dst, cap, &len, userns_visible(view, true, domain->gid));
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, userns_visible(view, true, domain->egid));
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, userns_visible(view, true, domain->saved_gid));
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, userns_visible(view, true, domain->egid));
  proc_append_str(dst, cap, &len, "\nGroups:");
  for (unsigned i = 0; i < domain->group_count; ++i) {
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, userns_visible(view, true, domain->groups[i]));
  }
  proc_append_str(dst, cap, &len, "\nVmSize:\t");
  proc_append_u64(dst, cap, &len, mem.virtual_pages * 4);
  proc_append_str(dst, cap, &len, " kB\nVmRSS:\t");
  proc_append_u64(dst, cap, &len, mem.resident_pages * 4);
  proc_append_str(dst, cap, &len, " kB\nVmSizePages:\t");
  proc_append_u64(dst, cap, &len, mem.virtual_pages);
  proc_append_str(dst, cap, &len, "\nVmRSSPages:\t");
  proc_append_u64(dst, cap, &len, mem.resident_pages);
  proc_append_str(dst, cap, &len, "\nMinFlt:\t");
  proc_append_u64(dst, cap, &len, mem.minor_faults);
  proc_append_str(dst, cap, &len, "\nMajFlt:\t");
  proc_append_u64(dst, cap, &len, mem.major_faults);
  proc_append_str(dst, cap, &len, "\nCpuTicks:\t");
  proc_append_u64(dst, cap, &len, task->thread ? task->thread->cpu_ticks : domain->cpu_ticks);
  proc_append_str(dst, cap, &len, "\nCwd:\t");
  proc_append_str(dst, cap, &len, domain->cwd);
  proc_append_str(dst, cap, &len, "\nUnsupportedSyscalls:\t");
  proc_append_u64(dst, cap, &len, domain->unsupported_syscalls);
  proc_append_str(dst, cap, &len, "\nLastUnsupportedSyscall:\t");
  proc_append_u64(dst, cap, &len, domain->last_unsupported_syscall);
  proc_append_str(dst, cap, &len, "\nUnsupportedIoctls:\t");
  proc_append_u64(dst, cap, &len, domain->unsupported_ioctls);
  proc_append_str(dst, cap, &len, "\nLastUnsupportedIoctl:\t");
  proc_append_u64(dst, cap, &len, domain->last_unsupported_ioctl);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t proc_pid_proc_stat_text(char *dst, size_t cap, const struct proc_task *task) {
  size_t len = 0;
  const struct domain *domain = task->domain;
  struct cell_memory_accounting mem = {0};
  (void)cell_memory_accounting(domain, &mem);
  uint64_t start_time = task->thread ? task->thread->start_ticks : domain->start_ticks;
  uint64_t utime = task->thread ? task->thread->cpu_ticks : domain->cpu_ticks;
  proc_append_u64(dst, cap, &len, task->thread ? task->thread->tid : domain->id);
  proc_append_str(dst, cap, &len, " (");
  proc_append_str(dst, cap, &len, domain->name);
  proc_append_str(dst, cap, &len, ") ");
  proc_append_char(dst, cap, &len, proc_state_char(task->thread ? task->thread->state : cell_domain_state(domain)));
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, (uint32_t)domain->parent_id);
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, domain->pgrp_id);
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, domain->session_id);
  proc_append_str(dst, cap, &len, " 0 0 0 ");
  proc_append_u64(dst, cap, &len, mem.minor_faults);
  proc_append_str(dst, cap, &len, " 0 ");
  proc_append_u64(dst, cap, &len, mem.major_faults);
  proc_append_str(dst, cap, &len, " 0 ");
  proc_append_u64(dst, cap, &len, utime);
  proc_append_str(dst, cap, &len, " 0 0 0 20 0 ");
  proc_append_u64(dst, cap, &len, domain->refcount);
  proc_append_str(dst, cap, &len, " 0 ");
  proc_append_u64(dst, cap, &len, start_time);
  proc_append_str(dst, cap, &len, " ");
  proc_append_u64(dst, cap, &len, mem.virtual_pages * PAGE_SIZE);
  proc_append_str(dst, cap, &len, " ");
  proc_append_u64(dst, cap, &len, mem.resident_pages);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
  return len;
}

struct proc_read {
  struct domain *domain;
  uint64_t buf, len, offset;
};

static int64_t read_cmdline(struct open_file *file, const struct proc_read *out) {
  const struct domain *target = cell_find_domain(file->node.proc.pid);
  if (target == NULL || target->zombie) { return 0; }
  const struct user_address_space *as = cell_domain_as_const(target);
  if (as == NULL || out->offset >= as->arg_end - as->arg_start) { return 0; }
  uint64_t src = as->arg_start + out->offset;
  uint64_t len = out->len;
  if (len > as->arg_end - src) { len = as->arg_end - src; }
  size_t done = 0;
  uint8_t bytes[512];
  while (done < len) {
    size_t chunk = PAGE_SIZE - (src + done) % PAGE_SIZE;
    if (chunk > sizeof(bytes)) { chunk = sizeof(bytes); }
    if (chunk > len - done) { chunk = len - done; }
    if (!vmm_copy_from_user(as, bytes, src + done, chunk)) { break; }
    if (!vmm_copy_to_user(cell_domain_as(out->domain), out->buf + done, bytes, chunk)) {
      if (done == 0) { return -14; }
      break;
    }
    done += chunk;
  }
  return (int64_t)done;
}

static size_t proc_pid_statm_text(char *dst, size_t cap, const struct proc_task *task) {
  size_t len = 0;
  const struct domain *domain = task->domain;
  struct cell_memory_accounting mem = {0};
  (void)cell_memory_accounting(domain, &mem);
  proc_append_u64(dst, cap, &len, mem.virtual_pages);
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, mem.resident_pages);
  proc_append_str(dst, cap, &len, " 0 0 0 ");
  proc_append_u64(dst, cap, &len, mem.virtual_pages > mem.resident_pages ? mem.virtual_pages - mem.resident_pages : 0);
  proc_append_str(dst, cap, &len, " 0\n");
  return len;
}

static size_t proc_pid_comm_text(char *dst, size_t cap, const struct proc_task *task) {
  size_t len = 0;
  const struct domain *domain = task->domain;
  proc_append_str(dst, cap, &len, domain->name);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t proc_pid_cwd_text(char *dst, size_t cap, const struct proc_task *task) {
  size_t len = 0;
  const struct domain *domain = task->domain;
  proc_append_str(dst, cap, &len, domain->cwd);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t proc_pid_exe_text(char *dst, size_t cap, const struct proc_task *task) {
  size_t len = 0;
  const struct domain *domain = task->domain;
  proc_append_str(dst, cap, &len, domain->exec_path[0] == '\0' ? "-" : domain->exec_path);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static int64_t read_text_device(const struct proc_read *out, const char *text, size_t text_len) {
  if (out->offset >= text_len) { return 0; }
  size_t chunk = text_len - out->offset;
  if (chunk > out->len) { chunk = out->len; }
  return vmm_copy_to_user(cell_domain_as(out->domain), out->buf, text + out->offset, chunk) ? (int64_t)chunk : -14;
}

static int64_t read_generated_device(const struct proc_read *out, size_t (*fill)(char *, size_t)) {
  char text[4096];
  size_t text_len = fill(text, sizeof(text));
  return read_text_device(out, text, text_len);
}

static bool sys_cpu_in_set(uint32_t cpu, enum ramfs_device device) {
  switch (device) {
  case RAMFS_DEV_SYS_CPU_POSSIBLE:
    return cpu < smp_possible_cpu_count();
  case RAMFS_DEV_SYS_CPU_PRESENT:
    return smp_cpu_present(cpu);
  case RAMFS_DEV_SYS_CPU_ONLINE:
    return smp_cpu_online(cpu);
  default:
    return false;
  }
}

static size_t sys_cpu_list_text(char *dst, size_t cap, enum ramfs_device device) {
  size_t len = 0;
  uint32_t limit = smp_possible_cpu_count();
  if (limit == 0) { limit = 1; }
  if (limit > SPORE_BOOT_CPU_MAX) { limit = SPORE_BOOT_CPU_MAX; }

  bool first = true;
  uint32_t range_start = 0;
  bool in_range = false;
  for (uint32_t cpu = 0; cpu <= limit; ++cpu) {
    bool included = cpu < limit && sys_cpu_in_set(cpu, device);
    if (included && !in_range) {
      range_start = cpu;
      in_range = true;
    }
    if ((!included || cpu == limit) && in_range) {
      uint32_t range_end = cpu - 1u;
      if (!first) { proc_append_char(dst, cap, &len, ','); }
      proc_append_u64(dst, cap, &len, range_start);
      if (range_end != range_start) {
        proc_append_char(dst, cap, &len, '-');
        proc_append_u64(dst, cap, &len, range_end);
      }
      first = false;
      in_range = false;
    }
  }
  if (first) { proc_append_char(dst, cap, &len, '0'); }
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static bool path_digit(char c) {
  return c >= '0' && c <= '9';
}

static bool sys_cpu_id_from_online_path(const char *path, uint32_t *out) {
  for (size_t i = 0; path[i] != '\0'; ++i) {
    if (path[i] != 'c' || path[i + 1] != 'p' || path[i + 2] != 'u' || !path_digit(path[i + 3])) { continue; }
    size_t j = i + 3;
    uint32_t value = 0;
    while (path_digit(path[j])) {
      value = value * 10u + (uint32_t)(path[j] - '0');
      ++j;
    }
    if (path[j] == '/' && path[j + 1] == 'o' && path[j + 2] == 'n' && path[j + 3] == 'l' && path[j + 4] == 'i' &&
        path[j + 5] == 'n' && path[j + 6] == 'e' && path[j + 7] == '\0') {
      *out = value;
      return true;
    }
  }
  return false;
}

static size_t sys_cpu_core_online_text(char *dst, size_t cap, const char *path) {
  size_t len = 0;
  uint32_t cpu = 0;
  bool online = sys_cpu_id_from_online_path(path, &cpu) && smp_cpu_online(cpu);
  proc_append_char(dst, cap, &len, online ? '1' : '0');
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static int64_t read_generated_pid_device(struct open_file *file, const struct proc_read *out,
                                         size_t (*fill)(char *, size_t, const struct proc_task *)) {
  struct proc_task task = {.domain = cell_find_domain(file->node.proc.pid)};
  if (task.domain == NULL) { return 0; }
  if (file->node.proc.tid != 0) {
    task.thread = cell_find_thread(file->node.proc.tid);
    if (task.thread == NULL || task.thread->domain != task.domain) { return 0; }
  }
  char text[4096];
  size_t text_len = fill(text, sizeof(text), &task);
  return read_text_device(out, text, text_len);
}

int64_t cell_procfs_read_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len, uint64_t offset) {
  struct proc_read out = {domain, buf, len, offset};
  switch (file->node.device) {
  case RAMFS_DEV_PROCINFO:
    return read_generated_device(&out, procinfo_text);
  case RAMFS_DEV_MEMINFO:
    return read_generated_device(&out, proc_meminfo_text);
  case RAMFS_DEV_CPUINFO:
    return read_generated_device(&out, proc_cpuinfo_text);
  case RAMFS_DEV_UPTIME:
    return read_generated_device(&out, proc_uptime_text);
  case RAMFS_DEV_LOADAVG:
    return read_generated_device(&out, proc_loadavg_text);
  case RAMFS_DEV_MOUNTS:
    return read_generated_device(&out, proc_mounts_text);
  case RAMFS_DEV_STAT:
    return read_generated_device(&out, proc_stat_text);
  case RAMFS_DEV_NET_DEV:
    return read_generated_device(&out, proc_net_dev_text);
  case RAMFS_DEV_KMSG:
    return read_generated_device(&out, proc_kmsg_text);
  case RAMFS_DEV_FILESYSTEMS:
    return read_generated_device(&out, proc_filesystems_text);
  case RAMFS_DEV_PARTITIONS:
    return read_generated_device(&out, proc_partitions_text);
  case RAMFS_DEV_DEVICES:
    return read_generated_device(&out, proc_devices_text);
  case RAMFS_DEV_FSSTATS:
    return read_generated_device(&out, proc_fsstats_text);
  case RAMFS_DEV_PROC_UID_MAP:
  case RAMFS_DEV_PROC_GID_MAP:
  case RAMFS_DEV_PROC_SETGROUPS: {
    const struct domain *target = cell_find_domain(file->node.proc.pid);
    if (!target) { return -3; }
    const struct user_namespace *ns = target->user_ns;
    if (file->node.device == RAMFS_DEV_PROC_SETGROUPS) {
      const char *text = ns && ns->groups_denied ? "deny\n" : "allow\n";
      return read_text_device(&out, text, kstrlen(text));
    }
    bool group = file->node.device == RAMFS_DEV_PROC_GID_MAP;
    if (!ns && !domain->user_ns) {
      const char text[] = "0 0 4294967295\n";
      return read_text_device(&out, text, sizeof(text) - 1);
    }
    if (!ns || (domain->user_ns && domain->user_ns != ns)) { return -1; }
    const struct id_map *map = group ? &ns->gid : &ns->uid;
    if (!map->mapped) { return 0; }
    char text[40] = {};
    size_t size = 0;
    proc_append_u64(text, sizeof(text), &size, map->inner);
    proc_append_char(text, sizeof(text), &size, ' ');
    proc_append_u64(text, sizeof(text), &size, map->outer);
    proc_append_str(text, sizeof(text), &size, " 1\n");
    return read_text_device(&out, text, size);
  }
  case RAMFS_DEV_PROC_PID_STAT:
    return read_generated_pid_device(file, &out, proc_pid_proc_stat_text);
  case RAMFS_DEV_PROC_PID_STATUS:
    return read_generated_pid_device(file, &out, proc_pid_status_text);
  case RAMFS_DEV_PROC_PID_CMDLINE:
    return read_cmdline(file, &out);
  case RAMFS_DEV_PROC_PID_STATM:
    return read_generated_pid_device(file, &out, proc_pid_statm_text);
  case RAMFS_DEV_PROC_PID_COMM:
    return read_generated_pid_device(file, &out, proc_pid_comm_text);
  case RAMFS_DEV_PROC_PID_MOUNTINFO:
    return read_generated_device(&out, proc_mountinfo_text);
  case RAMFS_DEV_PROC_PID_MOUNTS:
    return read_generated_device(&out, proc_mounts_text);
  case RAMFS_DEV_PROC_PID_CWD:
    return read_generated_pid_device(file, &out, proc_pid_cwd_text);
  case RAMFS_DEV_PROC_PID_EXE:
    return read_generated_pid_device(file, &out, proc_pid_exe_text);
  case RAMFS_DEV_FS_ROOT:
    return read_generated_device(&out, proc_fs_root_text);
  case RAMFS_DEV_FS_BOOT:
    return read_generated_device(&out, proc_fs_boot_text);
  case RAMFS_DEV_FS_RAM0:
    return read_generated_device(&out, proc_fs_ram0_text);
  case RAMFS_DEV_FS_TMP:
    return read_generated_device(&out, proc_fs_tmp_text);
  case RAMFS_DEV_SYS_CPU_POSSIBLE:
  case RAMFS_DEV_SYS_CPU_PRESENT:
  case RAMFS_DEV_SYS_CPU_ONLINE: {
    char text[128] = {0};
    size_t text_len = sys_cpu_list_text(text, sizeof(text), file->node.device);
    return read_text_device(&out, text, text_len);
  }
  case RAMFS_DEV_SYS_CPU_CORE_ONLINE: {
    char text[4] = {0};
    size_t text_len = sys_cpu_core_online_text(text, sizeof(text), file->path);
    return read_text_device(&out, text, text_len);
  }
  default:
    return -22;
  }
}
