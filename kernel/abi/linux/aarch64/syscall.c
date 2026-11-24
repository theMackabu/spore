#include "abi/linux/aarch64/syscall.h"
#include "arch/aarch64/platform.h"
#include "arch/aarch64/regs.h"
#include "arch/aarch64/signal_frame.h"
#include "numbers.h"
#include "init/app.h"
#include "abi/linux/shared/usercopy.h"
#include "fd/handlers.h"
#include "abi/linux/shared/fd/handlers.h"
#include "fs/handlers.h"
#include "abi/linux/shared/fs/handlers.h"
#include "abi/linux/shared/mm/handlers.h"
#include "net/handlers.h"
#include "abi/linux/shared/net/handlers.h"
#include "proc/handlers.h"
#include "abi/linux/shared/proc/handlers.h"
#include "abi/linux/shared/system/handlers.h"
#include "time/handlers.h"
#include "abi/linux/shared/time/handlers.h"
#include "lib/kprintf.h"
#include "mm/vmm.h"
#include "proc/domain.h"
#include "proc/eventfd.h"
#include "proc/fd.h"
#include "proc/futex.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/policy.h"
#include "proc/poll.h"
#include "proc/process.h"
#include "proc/signal.h"
#include "proc/snapshot.h"
#include "proc/thread.h"
#include "fs/vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  EPERM = 1,
  EFAULT = 14,
  EINVAL = 22,
  ENOSYS = 38,
  SIGINT = 2,
  SIGABRT = 6,
  SIGKILL = 9,
  SIGSEGV = 11,
  SIGPIPE = 13,
  SIGALRM = 14,
  SIGTERM = 15,
  SIGURG = 23,
};

constexpr int64_t SYSCALL_SWITCHED = CELL_SWITCHED;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
static bool signal_delivers_to_current(int signal) {
  return signal == SIGINT || signal == SIGABRT || signal == SIGKILL || signal == SIGSEGV || signal == SIGPIPE ||
         signal == SIGALRM || signal == SIGTERM || signal == SIGURG;
}

int64_t linux_aarch64_dispatch(struct trap_frame *f, uint64_t nr, const uint64_t args[6]) {
  static const void *linux_dispatch[] = {
    [SYS_GETCWD] = &&l_getcwd,
    [SYS_EVENTFD2] = &&l_eventfd2,
    [SYS_EPOLL_CREATE1] = &&l_epoll_create1,
    [SYS_EPOLL_CTL] = &&l_epoll_ctl,
    [SYS_EPOLL_PWAIT] = &&l_epoll_pwait,
    [SYS_DUP] = &&l_dup,
    [SYS_DUP3] = &&l_dup3,
    [SYS_FCNTL] = &&l_fcntl,
    [SYS_INOTIFY_INIT1] = &&l_inotify_init1,
    [SYS_INOTIFY_ADD_WATCH] = &&l_inotify_add_watch,
    [SYS_INOTIFY_RM_WATCH] = &&l_inotify_rm_watch,
    [SYS_FLOCK] = &&l_flock,
    [SYS_IOCTL] = &&l_ioctl,
    [SYS_MKDIRAT] = &&l_mkdirat,
    [SYS_UNLINKAT] = &&l_unlinkat,
    [SYS_SYMLINKAT] = &&l_symlinkat,
    [SYS_LINKAT] = &&l_linkat,
    [SYS_RENAMEAT] = &&l_renameat,
    [SYS_STATFS] = &&l_statfs,
    [SYS_FSTATFS] = &&l_fstatfs,
    [SYS_FACCESSAT] = &&l_faccessat,
    [SYS_FTRUNCATE] = &&l_ftruncate,
    [SYS_CHDIR] = &&l_chdir,
    [SYS_FCHDIR] = &&l_fchdir,
    [SYS_CHROOT] = &&l_chroot,
    [SYS_FCHMOD] = &&l_fchmod,
    [SYS_FCHMODAT] = &&l_fchmodat,
    [SYS_FCHOWNAT] = &&l_fchownat,
    [SYS_FCHOWN] = &&l_fchown,
    [SYS_OPENAT] = &&l_openat,
    [SYS_CLOSE] = &&l_close,
    [SYS_MKNODAT] = &&l_mknodat,
    [SYS_PIPE2] = &&l_pipe2,
    [SYS_GETDENTS64] = &&l_getdents64,
    [SYS_LSEEK] = &&l_lseek,
    [SYS_READ] = &&l_read,
    [SYS_WRITE] = &&l_write,
    [SYS_READV] = &&l_readv,
    [SYS_WRITEV] = &&l_writev,
    [SYS_PREAD64] = &&l_pread64,
    [SYS_PWRITE64] = &&l_pwrite64,
    [SYS_PSELECT6] = &&l_pselect6,
    [SYS_PPOLL] = &&l_ppoll,
    [SYS_READLINKAT] = &&l_readlinkat,
    [SYS_NEWFSTATAT] = &&l_newfstatat,
    [SYS_FSTAT] = &&l_fstat,
    [SYS_FSYNC] = &&l_fsync,
    [SYS_FDATASYNC] = &&l_fsync,
    [SYS_UTIMENSAT] = &&l_utimensat,
    [SYS_EXIT] = &&l_exit,
    [SYS_EXIT_GROUP] = &&l_exit_group,
    [SYS_SET_TID_ADDRESS] = &&l_set_tid_address,
    [SYS_FUTEX] = &&l_futex,
    [SYS_SET_ROBUST_LIST] = &&l_set_robust_list,
    [SYS_NANOSLEEP] = &&l_nanosleep,
    [SYS_SETITIMER] = &&l_setitimer,
    [SYS_CLOCK_GETTIME] = &&l_clock_gettime,
    [SYS_CLOCK_GETRES] = &&l_clock_getres,
    [SYS_CLOCK_NANOSLEEP] = &&l_clock_nanosleep,
    [SYS_SCHED_GETSCHEDULER] = &&l_sched_getscheduler,
    [SYS_SCHED_GETPARAM] = &&l_sched_getparam,
    [SYS_SCHED_GETAFFINITY] = &&l_sched_getaffinity,
    [SYS_SCHED_YIELD] = &&l_sched_yield,
    [SYS_KILL] = &&l_kill,
    [SYS_TKILL] = &&l_tkill,
    [SYS_TGKILL] = &&l_tgkill,
    [SYS_SIGALTSTACK] = &&l_sigaltstack,
    [SYS_RT_SIGACTION] = &&l_rt_sigaction,
    [SYS_RT_SIGPROCMASK] = &&l_rt_sigprocmask,
    [SYS_RT_SIGRETURN] = &&l_rt_sigreturn,
    [SYS_TIMES] = &&l_times,
    [SYS_SETGID] = &&l_setgid,
    [SYS_SETUID] = &&l_setuid,
    [SYS_SET_PGID] = &&l_setpgid,
    [SYS_GET_PGID] = &&l_getpgid,
    [SYS_GET_SID] = &&l_getsid,
    [SYS_SETSID] = &&l_setsid,
    [SYS_SETRESUID] = &&l_setresuid,
    [SYS_GETRESUID] = &&l_getresuid,
    [SYS_SETRESGID] = &&l_setresgid,
    [SYS_GETRESGID] = &&l_getresgid,
    [SYS_GETGROUPS] = &&l_getgroups,
    [SYS_SETGROUPS] = &&l_setgroups,
    [SYS_UNAME] = &&l_uname,
    [SYS_SETHOSTNAME] = &&l_sethostname,
    [SYS_GETRLIMIT] = &&l_getrlimit,
    [SYS_SETRLIMIT] = &&l_setrlimit,
    [SYS_GETRUSAGE] = &&l_getrusage,
    [SYS_UMASK] = &&l_umask,
    [SYS_PRCTL] = &&l_prctl,
    [SYS_GETTIMEOFDAY] = &&l_gettimeofday,
    [SYS_GETPID] = &&l_getpid,
    [SYS_GETPPID] = &&l_getppid,
    [SYS_GETUID] = &&l_getuid,
    [SYS_GETEUID] = &&l_geteuid,
    [SYS_GETGID] = &&l_getgid,
    [SYS_GETEGID] = &&l_getegid,
    [SYS_GETTID] = &&l_gettid,
    [SYS_SYSINFO] = &&l_sysinfo,
    [SYS_SOCKET] = &&l_socket,
    [SYS_SOCKETPAIR] = &&l_socketpair,
    [SYS_BIND] = &&l_bind,
    [SYS_LISTEN] = &&l_listen,
    [SYS_ACCEPT] = &&l_accept,
    [SYS_CONNECT] = &&l_connect,
    [SYS_GETSOCKNAME] = &&l_getsockname,
    [SYS_GETPEERNAME] = &&l_getpeername,
    [SYS_SENDTO] = &&l_sendto,
    [SYS_RECVFROM] = &&l_recvfrom,
    [SYS_SETSOCKOPT] = &&l_setsockopt,
    [SYS_GETSOCKOPT] = &&l_getsockopt,
    [SYS_SHUTDOWN] = &&l_shutdown,
    [SYS_SENDMSG] = &&l_sendmsg,
    [SYS_RECVMSG] = &&l_recvmsg,
    [SYS_ACCEPT4] = &&l_accept4,
    [SYS_BRK] = &&l_brk,
    [SYS_MUNMAP] = &&l_munmap,
    [SYS_MREMAP] = &&l_mremap,
    [SYS_CLONE] = &&l_clone,
    [SYS_EXECVE] = &&l_execve,
    [SYS_MMAP] = &&l_mmap,
    [SYS_MPROTECT] = &&l_mprotect,
    [SYS_MSYNC] = &&l_msync,
    [SYS_MADVISE] = &&l_madvise,
    [SYS_WAIT4] = &&l_wait4,
    [SYS_PRLIMIT64] = &&l_prlimit64,
    [SYS_RENAMEAT2] = &&l_renameat2,
    [SYS_GETRANDOM] = &&l_getrandom,
    [SYS_MEMFD_CREATE] = &&l_memfd_create,
    [SYS_MEMBARRIER] = &&l_membarrier,
    [SYS_STATX] = &&l_statx,
    [SYS_RSEQ] = &&l_rseq,
    [SYS_IO_URING_SETUP] = &&l_probe_enosys,
    [SYS_IO_URING_ENTER] = &&l_probe_enosys,
    [SYS_IO_URING_REGISTER] = &&l_probe_enosys,
    [SYS_CLONE3] = &&l_enosys,
    [SYS_CLOSE_RANGE] = &&l_close_range,
    [SYS_FACCESSAT2] = &&l_faccessat2,
    [SYS_FCHMODAT2] = &&l_fchmodat2,
  };
  static const void *spore_dispatch[] = {
    [SYS_SPORE_SNAPSHOT - SYS_SPORE_SNAPSHOT] = &&l_spore_snapshot,
    [SYS_SPORE_SPAWN - SYS_SPORE_SNAPSHOT] = &&l_spore_spawn,
    [SYS_SPORE_REAP - SYS_SPORE_SNAPSHOT] = &&l_spore_reap,
    [SYS_SPORE_RESIDENT - SYS_SPORE_SNAPSHOT] = &&l_spore_resident,
    [SYS_SPORE_SET_BUDGET - SYS_SPORE_SNAPSHOT] = &&l_spore_set_budget,
    [SYS_SPORE_APPLY_POLICY - SYS_SPORE_SNAPSHOT] = &&l_spore_apply_policy,
    [SYS_SPORE_SHUTDOWN - SYS_SPORE_SNAPSHOT] = &&l_spore_shutdown,
    [SYS_SPORE_PROCINFO - SYS_SPORE_SNAPSHOT] = &&l_spore_procinfo,
    [SYS_SPORE_FSINFO - SYS_SPORE_SNAPSHOT] = &&l_spore_fsinfo,
    [SYS_SPORE_MOUNTINFO - SYS_SPORE_SNAPSHOT] = &&l_spore_mountinfo,
    [SYS_SPORE_NET_CONFIG - SYS_SPORE_SNAPSHOT] = &&l_spore_net_config,
  };
  uint64_t a0 = args[0];
  uint64_t a1 = args[1];
  uint64_t a2 = args[2];
  uint64_t a3 = args[3];
  uint64_t a4 = args[4];
  uint64_t a5 = args[5];

  if (!cell_syscall_allowed(nr)) {
    kprintf("[spore] syscall denied nr=%d\n", (int)nr);
    return -(int64_t)EPERM;
  }

  if (nr < sizeof(linux_dispatch) / sizeof(linux_dispatch[0]) && linux_dispatch[nr] != NULL) {
    goto *linux_dispatch[nr];
  }
  if (nr >= SYS_SPORE_SNAPSHOT) {
    uint64_t spore_index = nr - SYS_SPORE_SNAPSHOT;
    if (spore_index < sizeof(spore_dispatch) / sizeof(spore_dispatch[0]) && spore_dispatch[spore_index] != NULL) {
      goto *spore_dispatch[spore_index];
    }
  }
  goto l_unknown;

l_sched_yield:
  trap_set_result(f, 0);
  cell_schedule(f);
  return SYSCALL_SWITCHED;
l_read:
  return sys_read(f, a0, a1, a2);
l_write:
  return sys_write(f, a0, a1, a2);
l_readv:
  return sys_vector_io(f, a0, a1, a2, false);
l_writev:
  return sys_vector_io(f, a0, a1, a2, true);
l_pread64:
  return sys_pread64(a0, a1, a2, a3);
l_pwrite64:
  return sys_pwrite64(a0, a1, a2, a3);
l_pselect6:
  return sys_pselect6(f, a0, a1, a2, a3, a4, a5);
l_ppoll:
  return sys_ppoll(f, a0, a1, a2, a3, a4);
l_eventfd2:
  return cell_fd_eventfd(a0, (int)a1);
l_epoll_create1:
  return cell_fd_epoll_create((int)a0);
l_epoll_ctl:
  return sys_epoll_ctl(a0, a1, a2, a3);
l_epoll_pwait:
  return sys_epoll_pwait(f, a0, a1, a2, a3, a4, a5);
l_inotify_init1:
  return sys_inotify_init1(a0);
l_inotify_add_watch:
  return sys_inotify_add_watch(a0, a1, a2);
l_inotify_rm_watch:
  return sys_inotify_rm_watch(a0, a1);
l_nanosleep:
  return sys_clock_nanosleep(f, 1, 0, a0, a1);
l_clock_nanosleep:
  return sys_clock_nanosleep(f, a0, a1, a2, a3);
l_exit:
  kprintf("[kernel] exit(%d)\n", (int)a0);
  cell_exit_thread_current((int)a0, f);
  return SYSCALL_SWITCHED;
l_exit_group:
  kprintf("[kernel] exit_group(%d)\n", (int)a0);
  cell_exit_group_current((int)a0, f);
  return SYSCALL_SWITCHED;
l_getpid:
  return cell_current_pid();
l_getppid:
  return cell_current_ppid();
l_gettid:
  return cell_current_tid();
l_setpgid:
  return cell_setpgid((int)a0, (int)a1);
l_getpgid:
  return cell_getpgid((int)a0);
l_getsid:
  return cell_getsid((int)a0);
l_setsid:
  return cell_setsid_current();
l_getuid:
  return cell_current_uid();
l_geteuid:
  return cell_current_euid();
l_getgid:
  return cell_current_gid();
l_getegid:
  return cell_current_egid();
l_setuid:
  return cell_setuid_current((uint32_t)a0);
l_setgid:
  return cell_setgid_current((uint32_t)a0);
l_clone:
  return sys_clone(f, a0, a1, a2, a3, a4);
l_futex: {
  int64_t rc = sys_futex(f, a0, a1, a2, a3, a4, a5);
  return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
}
l_execve:
  return sys_execve(f, a0, a1, a2);
l_wait4: {
  int rc = cell_wait4_options((int)a0, a1, (int)a2, a3, f);
  return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
}
l_kill:
  if ((int)a0 == cell_current_pid() && signal_delivers_to_current((int)a1)) {
    trap_set_result(f, 0);
    return cell_signal_current((int)a1, f) ? SYSCALL_SWITCHED : 0;
  }
  return cell_kill((int)a0, (int)a1);
l_tkill:
  if ((int)a0 == cell_current_tid() && signal_delivers_to_current((int)a1)) {
    trap_set_result(f, 0);
    return cell_signal_current((int)a1, f) ? SYSCALL_SWITCHED : 0;
  }
  return cell_tkill((int)a0, (int)a1);
l_tgkill:
  if ((int)a0 == cell_current_pid() && (int)a1 == cell_current_tid() && signal_delivers_to_current((int)a2)) {
    trap_set_result(f, 0);
    return cell_signal_current((int)a2, f) ? SYSCALL_SWITCHED : 0;
  }
  return cell_tgkill((int)a0, (int)a1, (int)a2);
l_rt_sigaction:
  return cell_rt_sigaction((int)a0, a1, a2, a3);
l_rt_sigprocmask:
  return sys_rt_sigprocmask(f, a0, a1, a2, a3);
l_rt_sigreturn:
  return cell_rt_sigreturn(f) == 0 ? SYSCALL_SWITCHED : -(int64_t)EFAULT;
l_sigaltstack:
  return sys_sigaltstack(f, a0, a1);
l_setitimer:
  return sys_setitimer(a0, a1, a2);
l_times:
  return sys_times(a0);
l_spore_snapshot:
  return snapshot_create_current();
l_spore_spawn:
  return snapshot_spawn((int)a0, a1, a2, f);
l_spore_reap: {
  int rc = snapshot_reap((int)a0, a1, f);
  return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
}
l_spore_resident:
  return (int64_t)cell_resident_pages(a0, a0 + a1);
l_spore_set_budget:
  return cell_set_budget((int)a0, a1);
l_spore_apply_policy: {
  char manifest[sizeof("fs:/tmp;exec:") + CELL_PATH_MAX];
  if (!syscall_copy_string_from_user(a0, manifest, sizeof(manifest))) { return -(int64_t)EFAULT; }
  int rc = cell_apply_policy(manifest);
  return rc == 0 ? 0 : -(int64_t)EPERM;
}
l_spore_shutdown:
  if (app_mode) { return -EPERM; }
  if (cell_current_euid() != 0) {
    kprintf("[kernel] shutdown denied uid=%d euid=%d\n", (int)cell_current_uid(), (int)cell_current_euid());
    return -(int64_t)EPERM;
  }
  (void)vfs_sync();
  if (a0 == 1) {
    kprintf("[kernel] reboot\n");
    aarch64_reboot();
  } else {
    kprintf("[kernel] shutdown\n");
    aarch64_poweroff();
  }
  return 0;
l_spore_procinfo:
  return sys_spore_procinfo(a0, a1);
l_spore_fsinfo:
  return sys_spore_fsinfo(a0);
l_spore_mountinfo:
  return sys_spore_mountinfo(a0, a1);
l_spore_net_config:
  return sys_spore_net_config(a0, a1);
l_brk:
  return sys_brk(a0);
l_mmap:
  return sys_mmap(a0, a1, a2, a3, a4, a5);
l_munmap:
  return sys_munmap(a0, a1);
l_mprotect:
  return sys_mprotect(a0, a1, a2);
l_msync:
  return sys_msync(a0, a1, a2);
l_madvise:
  return sys_madvise(a0, a1, a2);
l_mremap:
  return sys_mremap(a0, a1, a2, a3, a4);
l_memfd_create:
  return sys_memfd_create(a0, a1);
l_socket:
  return sys_socket(a0, a1, a2);
l_socketpair:
  return sys_socketpair(a0, a1, a2, a3);
l_bind:
  return sys_bind(a0, a1, a2);
l_listen:
  return sys_listen(a0, a1);
l_accept:
  return sys_accept4(f, a0, a1, a2, 0);
l_accept4:
  return sys_accept4(f, a0, a1, a2, a3);
l_connect:
  return sys_connect(f, a0, a1, a2);
l_sendto:
  return sys_sendto(f, a0, a1, a2, a3, a4, a5);
l_recvfrom:
  return sys_recvfrom(f, a0, a1, a2, a3, a4, a5);
l_sendmsg:
  return sys_sendmsg(f, a0, a1, a2);
l_recvmsg:
  return sys_recvmsg(f, a0, a1, a2);
l_getsockname:
  return sys_socket_name(a0, a1, a2, false);
l_getpeername:
  return sys_socket_name(a0, a1, a2, true);
l_setsockopt:
  return sys_setsockopt(a0, a1, a2, a3, a4);
l_getsockopt:
  return sys_getsockopt(a0, a1, a2, a3, a4);
l_shutdown:
  return sys_shutdown(a0, a1);
l_openat:
  return sys_openat(f, a0, a1, a2, a3);
l_close:
  return cell_fd_close((int)a0);
l_close_range:
  return sys_close_range(a0, a1, a2);
l_pipe2:
  return cell_fd_pipe2(a0, (int)a1);
l_lseek:
  return cell_fd_lseek((int)a0, (int64_t)a1, (int)a2);
l_fstat:
  return sys_fstat(a0, a1);
l_newfstatat:
  return sys_newfstatat(a0, a1, a2, a3);
l_faccessat:
  return sys_faccessat(a0, a1, a2, 0);
l_faccessat2:
  return sys_faccessat(a0, a1, a2, a3);
l_getdents64:
  return sys_getdents64(a0, a1, a2);
l_readlinkat:
  return sys_readlinkat(a0, a1, a2, a3);
l_utimensat:
  return sys_utimensat(a0, a1, a2, a3);
l_dup:
  return cell_fd_dup((int)a0, 0);
l_dup3:
  return cell_fd_dup3((int)a0, (int)a1, (int)a2);
l_fcntl:
  return sys_fcntl(a0, a1, a2);
l_flock: {
  return cell_fd_flock((int)a0, (int)a1, f);
}
l_getcwd:
  return sys_getcwd(a0, a1);
l_getrlimit:
  return sys_prlimit64(0, a0, 0, a1);
l_setrlimit:
  return sys_prlimit64(0, a0, a1, 0);
l_getrusage:
  return sys_getrusage((int)a0, a1);
l_prlimit64:
  return sys_prlimit64(a0, a1, a2, a3);
l_sysinfo:
  return sys_sysinfo(a0);
l_setresuid:
  return cell_setres_ids(false, a0, a1, a2);
l_getresuid:
  return sys_getres_ids(false, a0, a1, a2);
l_setresgid:
  return cell_setres_ids(true, a0, a1, a2);
l_getresgid:
  return sys_getres_ids(true, a0, a1, a2);
l_getgroups:
  return sys_getgroups((int)a0, a1);
l_setgroups:
  return sys_setgroups(a0, a1);
l_uname:
  return sys_uname(a0, "aarch64");
l_sethostname:
  return sys_sethostname(a0, a1);
l_umask:
  return cell_set_umask(a0);
l_prctl:
  return sys_prctl(a0, a1, a2, a3, a4);
l_chroot:
  return sys_chroot(a0);
l_set_robust_list:
  return cell_set_robust_list_current(a0);
l_sched_getscheduler:
  return sys_sched_getscheduler((int)a0);
l_sched_getparam:
  return sys_sched_getparam((int)a0, a1);
l_sched_getaffinity:
  return sys_sched_getaffinity(a2, a1);
l_set_tid_address:
  return cell_set_tid_address_current(a0);
l_ioctl:
  return sys_ioctl(a0, a1, a2);
l_mknodat:
  return sys_mknodat(a0, a1, a2);
l_mkdirat:
  return sys_mkdirat(a0, a1, a2);
l_linkat:
  return sys_linkat(a0, a1, a2, a3, a4);
l_symlinkat:
  return sys_symlinkat(a0, a1, a2);
l_unlinkat:
  return sys_unlinkat(a0, a1, a2);
l_renameat:
  return sys_renameat(a0, a1, a2, a3, 0);
l_renameat2:
  return sys_renameat(a0, a1, a2, a3, a4);
l_statfs:
  return sys_statfs(a0, a1);
l_fstatfs:
  return sys_fstatfs(a0, a1);
l_statx:
  return sys_statx(a0, a1, a2, a3, a4);
l_fchmod:
  return sys_fchmod(a0, a1);
l_fchmodat:
  return sys_fchmodat(a0, a1, a2, 0);
l_fchmodat2:
  return sys_fchmodat(a0, a1, a2, a3);
l_fchownat:
  return sys_fchownat(a0, a1, a2, a3, a4);
l_fchown:
  return sys_fchown(a0, a1, a2);
l_ftruncate:
  return sys_ftruncate(a0, a1);
l_chdir:
  return sys_chdir(a0);
l_fchdir:
  return sys_fchdir(a0);
l_fsync:
  return sys_fsync(a0);
l_getrandom:
  return sys_getrandom(a0, a1, a2);
l_clock_gettime:
  return sys_clock_gettime(a0, a1);
l_clock_getres:
  return sys_clock_getres(a0, a1);
l_gettimeofday:
  return sys_gettimeofday(a0, a1);
l_membarrier:
  return a0 == 0 ? 0 : -(int64_t)EINVAL;
l_rseq:
  cell_note_unsupported_syscall(nr);
  return -(int64_t)ENOSYS;
l_probe_enosys:
  cell_note_unsupported_syscall(nr);
  return -(int64_t)ENOSYS;
l_enosys:
  cell_note_unsupported_syscall(nr);
  return -(int64_t)ENOSYS;
l_unknown:
  cell_note_unsupported_syscall(nr);
  return -(int64_t)ENOSYS;
}
#pragma clang diagnostic pop

void linux_aarch64_syscall(struct trap_frame *frame) {
  int64_t result = linux_aarch64_dispatch(frame, trap_syscall_number(frame), frame->x);
  if (result != SYSCALL_SWITCHED) {
    trap_set_result(frame, (uint64_t)result);
    (void)cell_deliver_pending_signals_current(frame);
  }
}
