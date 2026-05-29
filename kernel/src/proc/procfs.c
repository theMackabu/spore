#include "proc/procfs.h"

#include "proc/procfs_format.h"
#include "proc/procfs_text.h"

#include "kstr.h"
#include "arch/aarch64/smp.h"
#include "mem.h"
#include "mm/pmm.h"
#include "proc/domain.h"
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
  case WAIT_PIPE:
    return "pipe";
  case WAIT_EPOLL:
    return "epoll";
  }
  return "?";
}

static const char *domain_state_text(const struct domain *domain) {
  if (domain == NULL) { return "unknown"; }
  if (domain->zombie) { return "zombie"; }
  bool blocked = false;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->domain != domain || thread->state == THREAD_UNUSED) { continue; }
    if (thread->state == THREAD_RUNNABLE) { return "running"; }
    if (thread->state == THREAD_BLOCKED) { blocked = true; }
  }
  return blocked ? "blocked" : "unknown";
}

static char domain_proc_state_char(const struct domain *domain) {
  const char *state = domain_state_text(domain);
  if (state[0] == 'r') { return 'R'; }
  if (state[0] == 'b') { return 'S'; }
  if (state[0] == 'z') { return 'Z'; }
  return '?';
}

static const char *domain_wait_text(const struct domain *domain) {
  if (domain == NULL || domain->zombie) { return "-"; }
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain == domain && thread->state == THREAD_BLOCKED) {
      return wait_reason_text(thread->wait_reason);
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
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
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

static const struct domain *domain_for_pid(int pid) {
  return cell_find_domain(pid);
}

static size_t proc_pid_status_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  struct cell_memory_accounting mem = {0};
  (void)cell_memory_accounting(domain, &mem);
  proc_append_str(dst, cap, &len, "Name:\t");
  proc_append_str(dst, cap, &len, domain->name);
  proc_append_str(dst, cap, &len, "\nState:\t");
  proc_append_str(dst, cap, &len, domain_state_text(domain));
  proc_append_str(dst, cap, &len, "\nPid:\t");
  proc_append_u64(dst, cap, &len, (uint32_t)domain->id);
  proc_append_str(dst, cap, &len, "\nPPid:\t");
  proc_append_u64(dst, cap, &len, (uint32_t)domain->parent_id);
  proc_append_str(dst, cap, &len, "\nUid:\t");
  proc_append_u64(dst, cap, &len, domain->uid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->euid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->uid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->euid);
  proc_append_str(dst, cap, &len, "\nGid:\t");
  proc_append_u64(dst, cap, &len, domain->gid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->egid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->gid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->egid);
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
  proc_append_u64(dst, cap, &len, domain->cpu_ticks);
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

static size_t proc_pid_proc_stat_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  struct cell_memory_accounting mem = {0};
  (void)cell_memory_accounting(domain, &mem);
  uint64_t start_time = domain->start_ticks;
  uint64_t utime = domain->cpu_ticks;
  proc_append_u64(dst, cap, &len, (uint32_t)domain->id);
  proc_append_str(dst, cap, &len, " (");
  proc_append_str(dst, cap, &len, domain->name);
  proc_append_str(dst, cap, &len, ") ");
  proc_append_char(dst, cap, &len, domain_proc_state_char(domain));
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, (uint32_t)domain->parent_id);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 ");
  proc_append_u64(dst, cap, &len, mem.minor_faults);
  proc_append_str(dst, cap, &len, " 0 ");
  proc_append_u64(dst, cap, &len, mem.major_faults);
  proc_append_str(dst, cap, &len, " 0 ");
  proc_append_u64(dst, cap, &len, utime);
  proc_append_str(dst, cap, &len, " 0 0 0 20 0 1 0 ");
  proc_append_u64(dst, cap, &len, start_time);
  proc_append_str(dst, cap, &len, " ");
  proc_append_u64(dst, cap, &len, mem.virtual_pages * PAGE_SIZE);
  proc_append_str(dst, cap, &len, " ");
  proc_append_u64(dst, cap, &len, mem.resident_pages);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
  return len;
}

static size_t proc_pid_cmdline_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, domain->exec_path[0] == '\0' ? domain->name : domain->exec_path);
  proc_append_char(dst, cap, &len, '\0');
  return len;
}

static size_t proc_pid_statm_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
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

static size_t proc_pid_comm_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, domain->name);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t proc_pid_cwd_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, domain->cwd);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t proc_pid_exe_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, domain->exec_path[0] == '\0' ? "-" : domain->exec_path);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static int64_t read_generated_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len,
                                     size_t (*fill)(char *, size_t)) {
  char text[4096] = {0};
  size_t text_len = fill(text, sizeof(text));
  if (file->offset >= text_len) { return 0; }
  size_t chunk = text_len - (size_t)file->offset;
  if (chunk > len) { chunk = (size_t)len; }
  if (!vmm_copy_to_user(cell_domain_as(domain), buf, text + file->offset, chunk)) { return -14; }
  file->offset += chunk;
  return (int64_t)chunk;
}

static int64_t read_text_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len,
                                const char *text, size_t text_len) {
  if (file->offset >= text_len) { return 0; }
  size_t chunk = text_len - (size_t)file->offset;
  if (chunk > len) { chunk = (size_t)len; }
  if (!vmm_copy_to_user(cell_domain_as(domain), buf, text + file->offset, chunk)) { return -14; }
  file->offset += chunk;
  return (int64_t)chunk;
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

static int64_t read_generated_pid_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len,
                                         size_t (*fill)(char *, size_t, int)) {
  char text[4096] = {0};
  size_t text_len = fill(text, sizeof(text), file->node.proc_pid);
  if (file->offset >= text_len) { return 0; }
  size_t chunk = text_len - (size_t)file->offset;
  if (chunk > len) { chunk = (size_t)len; }
  if (!vmm_copy_to_user(cell_domain_as(domain), buf, text + file->offset, chunk)) { return -14; }
  file->offset += chunk;
  return (int64_t)chunk;
}

size_t cell_proc_info(struct proc_info *out, size_t max) {
  size_t count = 0;
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    const struct domain *domain = cell_domain_slot(i);
    if (domain == NULL || !domain->used) { continue; }
    if (count < max && out != NULL) {
      struct cell_memory_accounting mem = {0};
      (void)cell_memory_accounting(domain, &mem);
      struct proc_info info = {
        .pid = (uint32_t)domain->id,
        .tid = (uint32_t)(cell_thread_for_domain((struct domain *)domain) == NULL
                            ? 0
                            : cell_thread_for_domain((struct domain *)domain)->tid),
        .ppid = (uint32_t)domain->parent_id,
        .state = (uint32_t)(domain->zombie
                              ? THREAD_ZOMBIE
                              : (str_eq(domain_state_text(domain), "blocked") ? THREAD_BLOCKED : THREAD_RUNNABLE)),
        .wait_reason = 0,
        .resident_pages = mem.resident_pages,
        .virtual_pages = mem.virtual_pages,
        .minor_faults = mem.minor_faults,
        .major_faults = mem.major_faults,
        .cpu_ticks = domain->cpu_ticks,
        .start_ticks = domain->start_ticks,
        .remaining_ticks = domain->budget.remaining_ticks,
        .max_ticks = domain->budget.max_ticks,
      };
      copy_cstr(info.name, sizeof(info.name), domain->name);
      copy_cstr(info.exec_path, sizeof(info.exec_path), domain->exec_path);
      copy_cstr(info.argv0, sizeof(info.argv0), domain->argv0);
      copy_cstr(info.cmdline, sizeof(info.cmdline), domain->cmdline);
      copy_cstr(info.cwd, sizeof(info.cwd), domain->cwd);
      out[count] = info;
    }
    ++count;
  }
  return count;
}

int64_t cell_procfs_read_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len) {
  switch (file->node.device) {
  case RAMFS_DEV_PROCINFO:
    return read_generated_device(file, domain, buf, len, procinfo_text);
  case RAMFS_DEV_MEMINFO:
    return read_generated_device(file, domain, buf, len, proc_meminfo_text);
  case RAMFS_DEV_CPUINFO:
    return read_generated_device(file, domain, buf, len, proc_cpuinfo_text);
  case RAMFS_DEV_UPTIME:
    return read_generated_device(file, domain, buf, len, proc_uptime_text);
  case RAMFS_DEV_LOADAVG:
    return read_generated_device(file, domain, buf, len, proc_loadavg_text);
  case RAMFS_DEV_MOUNTS:
    return read_generated_device(file, domain, buf, len, proc_mounts_text);
  case RAMFS_DEV_STAT:
    return read_generated_device(file, domain, buf, len, proc_stat_text);
  case RAMFS_DEV_NET_DEV:
    return read_generated_device(file, domain, buf, len, proc_net_dev_text);
  case RAMFS_DEV_KMSG:
    return read_generated_device(file, domain, buf, len, proc_kmsg_text);
  case RAMFS_DEV_FILESYSTEMS:
    return read_generated_device(file, domain, buf, len, proc_filesystems_text);
  case RAMFS_DEV_PARTITIONS:
    return read_generated_device(file, domain, buf, len, proc_partitions_text);
  case RAMFS_DEV_DEVICES:
    return read_generated_device(file, domain, buf, len, proc_devices_text);
  case RAMFS_DEV_FSSTATS:
    return read_generated_device(file, domain, buf, len, proc_fsstats_text);
  case RAMFS_DEV_PROC_PID_STAT:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_proc_stat_text);
  case RAMFS_DEV_PROC_PID_STATUS:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_status_text);
  case RAMFS_DEV_PROC_PID_CMDLINE:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_cmdline_text);
  case RAMFS_DEV_PROC_PID_STATM:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_statm_text);
  case RAMFS_DEV_PROC_PID_COMM:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_comm_text);
  case RAMFS_DEV_PROC_PID_MOUNTS:
    return read_generated_device(file, domain, buf, len, proc_mounts_text);
  case RAMFS_DEV_PROC_PID_CWD:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_cwd_text);
  case RAMFS_DEV_PROC_PID_EXE:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_exe_text);
  case RAMFS_DEV_FS_ROOT:
    return read_generated_device(file, domain, buf, len, proc_fs_root_text);
  case RAMFS_DEV_FS_BOOT:
    return read_generated_device(file, domain, buf, len, proc_fs_boot_text);
  case RAMFS_DEV_FS_RAM0:
    return read_generated_device(file, domain, buf, len, proc_fs_ram0_text);
  case RAMFS_DEV_FS_TMP:
    return read_generated_device(file, domain, buf, len, proc_fs_tmp_text);
  case RAMFS_DEV_SYS_CPU_POSSIBLE:
  case RAMFS_DEV_SYS_CPU_PRESENT:
  case RAMFS_DEV_SYS_CPU_ONLINE: {
    char text[128] = {0};
    size_t text_len = sys_cpu_list_text(text, sizeof(text), file->node.device);
    return read_text_device(file, domain, buf, len, text, text_len);
  }
  case RAMFS_DEV_SYS_CPU_CORE_ONLINE: {
    char text[4] = {0};
    size_t text_len = sys_cpu_core_online_text(text, sizeof(text), file->path);
    return read_text_device(file, domain, buf, len, text, text_len);
  }
  default:
    return -22;
  }
}
