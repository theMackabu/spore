#include "arch/aarch64/exceptions.h"

#include "arch/aarch64/regs.h"
#include "arch/aarch64/syscall_handlers.h"
#include "cell.h"
#include "elf/loader.h"
#include "exec/stack.h"
#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "net.h"
#include "pl011.h"
#include "ramfs.h"
#include "random.h"
#include "spore_version.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  EC_SVC_A64 = 0x15,
  SYS_GETCWD = 17,
  SYS_EVENTFD2 = 19,
  SYS_EPOLL_CREATE1 = 20,
  SYS_EPOLL_CTL = 21,
  SYS_EPOLL_PWAIT = 22,
  SYS_DUP = 23,
  SYS_DUP3 = 24,
  SYS_FCNTL = 25,
  SYS_IOCTL = 29,
  SYS_MKNODAT = 33,
  SYS_MKDIRAT = 34,
  SYS_UNLINKAT = 35,
  SYS_SYMLINKAT = 36,
  SYS_LINKAT = 37,
  SYS_RENAMEAT = 38,
  SYS_STATFS = 43,
  SYS_FSTATFS = 44,
  SYS_FTRUNCATE = 46,
  SYS_FACCESSAT = 48,
  SYS_CHDIR = 49,
  SYS_FCHDIR = 50,
  SYS_CHROOT = 51,
  SYS_FCHMOD = 52,
  SYS_FCHMODAT = 53,
  SYS_FCHOWNAT = 54,
  SYS_FCHOWN = 55,
  SYS_OPENAT = 56,
  SYS_CLOSE = 57,
  SYS_PIPE2 = 59,
  SYS_GETDENTS64 = 61,
  SYS_LSEEK = 62,
  SYS_READ = 63,
  SYS_WRITE = 64,
  SYS_READV = 65,
  SYS_WRITEV = 66,
  SYS_PSELECT6 = 72,
  SYS_PPOLL = 73,
  SYS_READLINKAT = 78,
  SYS_NEWFSTATAT = 79,
  SYS_FSTAT = 80,
  SYS_FSYNC = 82,
  SYS_EXIT = 93,
  SYS_EXIT_GROUP = 94,
  SYS_SET_TID_ADDRESS = 96,
  SYS_FUTEX = 98,
  SYS_SET_ROBUST_LIST = 99,
  SYS_NANOSLEEP = 101,
  SYS_SETITIMER = 103,
  SYS_CLOCK_GETTIME = 113,
  SYS_CLOCK_GETRES = 114,
  SYS_CLOCK_NANOSLEEP = 115,
  SYS_SCHED_GETAFFINITY = 123,
  SYS_SCHED_YIELD = 124,
  SYS_KILL = 129,
  SYS_TKILL = 130,
  SYS_TGKILL = 131,
  SYS_SIGALTSTACK = 132,
  SYS_RT_SIGACTION = 134,
  SYS_RT_SIGPROCMASK = 135,
  SYS_RT_SIGRETURN = 139,
  SYS_TIMES = 153,
  SYS_SETGID = 144,
  SYS_SETUID = 146,
  SYS_SET_PGID = 154,
  SYS_GET_PGID = 155,
  SYS_GET_SID = 156,
  SYS_SETSID = 157,
  SYS_UNAME = 160,
  SYS_SETHOSTNAME = 161,
  SYS_GETRLIMIT = 163,
  SYS_SETRLIMIT = 164,
  SYS_GETRUSAGE = 165,
  SYS_UMASK = 166,
  SYS_PRCTL = 167,
  SYS_GETTIMEOFDAY = 169,
  SYS_GETPID = 172,
  SYS_GETPPID = 173,
  SYS_GETUID = 174,
  SYS_GETEUID = 175,
  SYS_GETGID = 176,
  SYS_GETEGID = 177,
  SYS_GETTID = 178,
  SYS_SYSINFO = 179,
  SYS_SOCKET = 198,
  SYS_BIND = 200,
  SYS_LISTEN = 201,
  SYS_ACCEPT = 202,
  SYS_CONNECT = 203,
  SYS_GETSOCKNAME = 204,
  SYS_SENDTO = 206,
  SYS_RECVFROM = 207,
  SYS_SETSOCKOPT = 208,
  SYS_GETSOCKOPT = 209,
  SYS_SENDMSG = 211,
  SYS_RECVMSG = 212,
  SYS_BRK = 214,
  SYS_MUNMAP = 215,
  SYS_MREMAP = 216,
  SYS_CLONE = 220,
  SYS_EXECVE = 221,
  SYS_MMAP = 222,
  SYS_MPROTECT = 226,
  SYS_MADVISE = 233,
  SYS_WAIT4 = 260,
  SYS_PRLIMIT64 = 261,
  SYS_RENAMEAT2 = 276,
  SYS_GETRANDOM = 278,
  SYS_MEMBARRIER = 283,
  SYS_STATX = 291,
  SYS_RSEQ = 293,
  SYS_IO_URING_SETUP = 425,
  SYS_IO_URING_ENTER = 426,
  SYS_IO_URING_REGISTER = 427,
  SYS_CLONE3 = 435,
  SYS_FACCESSAT2 = 439,
  SYS_SPORE_SNAPSHOT = 0x4000,
  SYS_SPORE_SPAWN = 0x4001,
  SYS_SPORE_REAP = 0x4002,
  SYS_SPORE_RESIDENT = 0x4003,
  SYS_SPORE_SET_BUDGET = 0x4004,
  SYS_SPORE_APPLY_POLICY = 0x4005,
  SYS_SPORE_SHUTDOWN = 0x4006,
  SYS_SPORE_PROCINFO = 0x4007,
  SYS_SPORE_FSINFO = 0x4008,
  SYS_SPORE_MOUNTINFO = 0x4009,
  SYS_SPORE_NET_CONFIG = 0x400a,
  F_DUPFD = 0,
  F_GETFD = 1,
  F_SETFD = 2,
  F_GETFL = 3,
  F_SETFL = 4,
  FD_CLOEXEC = 1,
  DT_FIFO = 1,
  ENOENT = 2,
  EPERM = 1,
  EBADF = 9,
  EACCES = 13,
  EFAULT = 14,
  EINVAL = 22,
  ENOTDIR = 20,
  ENOSYS = 38,
  ENAMETOOLONG = 36,
};

#define SYSCALL_SWITCHED ((int64_t)CELL_SWITCHED)
#define INTERP_LOAD_BASE 0x0000006000000000ull

struct fs_info64 {
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
  uint64_t inode_count;
  uint64_t free_inodes;
};

struct mount_info64 {
  char source[32];
  char target[32];
  char fstype[16];
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
};

static void system_poweroff(void) {
  __asm__ volatile("mov x0, #0x0008\n"
                   "movk x0, #0x8400, lsl #16\n"
                   "hvc #0\n"
                   :
                   :
                   : "x0", "memory");
  for (;;) {
    __asm__ volatile("wfe");
  }
}

static void system_reboot(void) {
  __asm__ volatile("mov x0, #0x0009\n"
                   "movk x0, #0x8400, lsl #16\n"
                   "hvc #0\n"
                   :
                   :
                   : "x0", "memory");
  for (;;) {
    __asm__ volatile("wfe");
  }
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
static int64_t dispatch(struct trap_frame *f) {
  static const void *linux_dispatch[] = {
    [SYS_GETCWD] = &&l_getcwd,
    [SYS_EVENTFD2] = &&l_eventfd2,
    [SYS_EPOLL_CREATE1] = &&l_epoll_create1,
    [SYS_EPOLL_CTL] = &&l_epoll_ctl,
    [SYS_EPOLL_PWAIT] = &&l_epoll_pwait,
    [SYS_DUP] = &&l_dup,
    [SYS_DUP3] = &&l_dup3,
    [SYS_FCNTL] = &&l_fcntl,
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
    [SYS_PSELECT6] = &&l_pselect6,
    [SYS_PPOLL] = &&l_ppoll,
    [SYS_READLINKAT] = &&l_readlinkat,
    [SYS_NEWFSTATAT] = &&l_newfstatat,
    [SYS_FSTAT] = &&l_fstat,
    [SYS_FSYNC] = &&l_fsync,
    [SYS_EXIT] = &&l_exit,
    [SYS_EXIT_GROUP] = &&l_exit_group,
    [SYS_SET_TID_ADDRESS] = &&l_set_tid_address,
    [SYS_FUTEX] = &&l_futex,
    [SYS_SET_ROBUST_LIST] = &&l_set_robust_list,
    [SYS_NANOSLEEP] = &&l_nanosleep,
    [SYS_SETITIMER] = &&l_zero,
    [SYS_CLOCK_GETTIME] = &&l_clock_gettime,
    [SYS_CLOCK_GETRES] = &&l_clock_getres,
    [SYS_CLOCK_NANOSLEEP] = &&l_clock_nanosleep,
    [SYS_SCHED_GETAFFINITY] = &&l_sched_getaffinity,
    [SYS_SCHED_YIELD] = &&l_sched_yield,
    [SYS_KILL] = &&l_kill,
    [SYS_TKILL] = &&l_tkill,
    [SYS_TGKILL] = &&l_tgkill,
    [SYS_SIGALTSTACK] = &&l_sigaltstack,
    [SYS_RT_SIGACTION] = &&l_rt_sigaction,
    [SYS_RT_SIGPROCMASK] = &&l_zero,
    [SYS_RT_SIGRETURN] = &&l_rt_sigreturn,
    [SYS_TIMES] = &&l_times,
    [SYS_SETGID] = &&l_setgid,
    [SYS_SETUID] = &&l_setuid,
    [SYS_SET_PGID] = &&l_setpgid,
    [SYS_GET_PGID] = &&l_getpgid,
    [SYS_GET_SID] = &&l_getsid,
    [SYS_SETSID] = &&l_setsid,
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
    [SYS_BIND] = &&l_bind,
    [SYS_LISTEN] = &&l_listen,
    [SYS_ACCEPT] = &&l_accept,
    [SYS_CONNECT] = &&l_connect,
    [SYS_GETSOCKNAME] = &&l_getsockname,
    [SYS_SENDTO] = &&l_sendto,
    [SYS_RECVFROM] = &&l_recvfrom,
    [SYS_SETSOCKOPT] = &&l_zero,
    [SYS_GETSOCKOPT] = &&l_getsockopt,
    [SYS_SENDMSG] = &&l_sendmsg,
    [SYS_RECVMSG] = &&l_recvmsg,
    [SYS_BRK] = &&l_brk,
    [SYS_MUNMAP] = &&l_munmap,
    [SYS_MREMAP] = &&l_mremap,
    [SYS_CLONE] = &&l_clone,
    [SYS_EXECVE] = &&l_execve,
    [SYS_MMAP] = &&l_mmap,
    [SYS_MPROTECT] = &&l_mprotect,
    [SYS_MADVISE] = &&l_madvise,
    [SYS_WAIT4] = &&l_wait4,
    [SYS_PRLIMIT64] = &&l_prlimit64,
    [SYS_RENAMEAT2] = &&l_renameat2,
    [SYS_GETRANDOM] = &&l_getrandom,
    [SYS_MEMBARRIER] = &&l_membarrier,
    [SYS_STATX] = &&l_statx,
    [SYS_RSEQ] = &&l_rseq,
    [SYS_IO_URING_SETUP] = &&l_probe_enosys,
    [SYS_IO_URING_ENTER] = &&l_probe_enosys,
    [SYS_IO_URING_REGISTER] = &&l_probe_enosys,
    [SYS_CLONE3] = &&l_enosys,
    [SYS_FACCESSAT2] = &&l_faccessat2,
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
  uint64_t nr = f->x[8];
  uint64_t a0 = f->x[0];
  uint64_t a1 = f->x[1];
  uint64_t a2 = f->x[2];
  uint64_t a3 = f->x[3];
  uint64_t a4 = f->x[4];
  uint64_t a5 = f->x[5];

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
  cell_schedule(f);
  return SYSCALL_SWITCHED;
l_read:
  return sys_read(f, a0, a1, a2);
l_write:
  return sys_write(f, a0, a1, a2);
l_readv:
  return sys_readv(f, a0, a1, a2);
l_writev:
  return sys_writev(f, a0, a1, a2);
l_pselect6:
  return sys_pselect6(f, a0, a1, a2, a3, a4);
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
l_nanosleep:
  return sys_nanosleep(f, a0, a1);
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
  return cell_setuid_current((uint32_t)a0) == 0 ? 0 : -(int64_t)EPERM;
l_setgid:
  return cell_setgid_current((uint32_t)a0) == 0 ? 0 : -(int64_t)EPERM;
l_zero:
  return 0;
l_clone:
  return sys_clone(f, a0, a1, a2, a3, a4);
l_futex: {
  int64_t rc = sys_futex(f, a0, a1, a2, a3);
  return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
}
l_execve:
  return sys_execve(f, a0, a1, a2);
l_wait4: {
  int rc = cell_wait4_options((int)a0, a1, (int)a2, f);
  return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
}
l_kill:
  if ((int)a0 == cell_current_pid() &&
      ((int)a1 == 2 || (int)a1 == 6 || (int)a1 == 9 || (int)a1 == 11 || (int)a1 == 15)) {
    cell_signal_current((int)a1, f);
    return SYSCALL_SWITCHED;
  }
  return cell_kill((int)a0, (int)a1);
l_tkill:
  if ((int)a0 == cell_current_tid() &&
      ((int)a1 == 2 || (int)a1 == 6 || (int)a1 == 9 || (int)a1 == 11 || (int)a1 == 15)) {
    cell_signal_current((int)a1, f);
    return SYSCALL_SWITCHED;
  }
  return cell_tkill((int)a0, (int)a1);
l_tgkill:
  if ((int)a0 == cell_current_pid() && (int)a1 == cell_current_tid() &&
      ((int)a2 == 2 || (int)a2 == 6 || (int)a2 == 9 || (int)a2 == 11 || (int)a2 == 15)) {
    cell_signal_current((int)a2, f);
    return SYSCALL_SWITCHED;
  }
  return cell_tgkill((int)a0, (int)a1, (int)a2);
l_rt_sigaction:
  return cell_rt_sigaction((int)a0, a1, a2, a3);
l_rt_sigreturn:
  return cell_rt_sigreturn(f) == 0 ? SYSCALL_SWITCHED : -(int64_t)EFAULT;
l_sigaltstack:
  return sys_sigaltstack(a0, a1);
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
  char manifest[64];
  if (!syscall_copy_string_from_user(a0, manifest, sizeof(manifest))) { return -(int64_t)EFAULT; }
  int rc = cell_apply_policy(manifest);
  return rc == 0 ? 0 : -(int64_t)EPERM;
}
l_spore_shutdown:
  if (cell_current_euid() != 0) {
    kprintf("[kernel] shutdown denied uid=%d euid=%d\n", (int)cell_current_uid(), (int)cell_current_euid());
    return -(int64_t)EPERM;
  }
  if (a0 == 1) {
    kprintf("[kernel] reboot\n");
    system_reboot();
  } else {
    kprintf("[kernel] shutdown\n");
    system_poweroff();
  }
  return 0;
l_spore_procinfo: {
  size_t max = a1 / sizeof(struct proc_info);
  if (a0 == 0 || max == 0) { return (int64_t)cell_proc_info(NULL, 0); }
  struct proc_info infos[MAX_THREADS];
  size_t count = cell_proc_info(infos, max);
  size_t copy = count < max ? count : max;
  return syscall_user_writable(a0, copy * sizeof(infos[0])) && vmm_copy_to_user(syscall_active_as(), a0, infos, copy * sizeof(infos[0]))
           ? (int64_t)count
           : -(int64_t)EFAULT;
}
l_spore_fsinfo: {
  struct vfs_fs_info raw;
  if (!vfs_fs_info(&raw)) { return -(int64_t)EINVAL; }
  struct fs_info64 info = {
    .block_size = raw.block_size,
    .block_count = raw.block_count,
    .free_blocks = raw.free_blocks,
    .inode_count = raw.inode_count,
    .free_inodes = raw.free_inodes,
  };
  return syscall_user_writable(a0, sizeof(info)) && vmm_copy_to_user(syscall_active_as(), a0, &info, sizeof(info)) ? 0
                                                                                                   : -(int64_t)EFAULT;
}
l_spore_mountinfo: {
  size_t max = a1 / sizeof(struct mount_info64);
  if (a0 == 0 || max == 0) { return (int64_t)vfs_mount_info(NULL, 0); }
  struct vfs_mount_info raw[8];
  size_t count = vfs_mount_info(raw, max < 8 ? max : 8);
  size_t copy = count < max ? count : max;
  struct mount_info64 out[8];
  for (size_t i = 0; i < copy; ++i) {
    kmemset(&out[i], 0, sizeof(out[i]));
    kmemcpy(out[i].source, raw[i].source, sizeof(out[i].source));
    kmemcpy(out[i].target, raw[i].target, sizeof(out[i].target));
    kmemcpy(out[i].fstype, raw[i].fstype, sizeof(out[i].fstype));
    out[i].block_size = raw[i].block_size;
    out[i].block_count = raw[i].block_count;
    out[i].free_blocks = raw[i].free_blocks;
  }
  return syscall_user_writable(a0, copy * sizeof(out[0])) && vmm_copy_to_user(syscall_active_as(), a0, out, copy * sizeof(out[0]))
           ? (int64_t)count
           : -(int64_t)EFAULT;
}
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
l_madvise:
  return sys_madvise(a0, a1, a2);
l_mremap:
  return sys_mremap(a0, a1, a2, a3, a4);
l_socket:
  return sys_socket(a0, a1, a2);
l_bind:
  return sys_bind(a0, a1, a2);
l_listen:
  return sys_listen(a0, a1);
l_accept:
  return sys_accept(f, a0, a1, a2);
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
  return sys_getsockname(a0, a1, a2);
l_getsockopt:
  return sys_getsockopt(a0, a1, a2, a3, a4);
l_openat:
  return sys_openat(a0, a1, a2);
l_close:
  return cell_fd_close((int)a0);
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
l_dup:
  return cell_fd_dup((int)a0, 0);
l_dup3:
  return cell_fd_dup3((int)a0, (int)a1, (int)a2);
l_fcntl:
  if (a1 == F_DUPFD) { return cell_fd_dup((int)a0, (int)a2); }
  if (a1 == F_GETFD) { return cell_fd_get_fd_flags((int)a0); }
  if (a1 == F_SETFD) { return cell_fd_set_fd_flags((int)a0, (int)a2); }
  if (a1 == F_GETFL) {
    int flags = cell_fd_get_flags((int)a0);
    return flags;
  }
  if (a1 == F_SETFL) { return cell_fd_set_flags((int)a0, (int)a2); }
  return -(int64_t)EINVAL;
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
l_uname:
  return sys_uname(a0);
l_sethostname:
  return sys_sethostname(a0, a1);
l_umask:
  return 022;
l_prctl:
  return sys_prctl(a0, a1, a2, a3, a4);
l_chroot:
  return sys_chroot(a0);
l_set_robust_list:
  return cell_set_robust_list_current(a0);
l_sched_getaffinity:
  return sys_sched_getaffinity(a2, a1);
l_set_tid_address:
  return cell_set_tid_address_current(a0);
l_ioctl:
  return sys_ioctl(a0, a1, a2);
l_mknodat:
  return sys_mknodat(a0, a1, a2);
l_mkdirat:
  return sys_mkdirat(a0, a1);
l_linkat:
  return sys_linkat(a0, a1, a2, a3, a4);
l_symlinkat:
  return sys_symlinkat(a0, a1, a2);
l_unlinkat:
  return sys_unlinkat(a0, a1);
l_renameat:
  return sys_renameat(a0, a1, a2, a3);
l_renameat2:
  return a4 == 0 ? sys_renameat(a0, a1, a2, a3) : -(int64_t)EINVAL;
l_statfs:
  return sys_statfs(a0, a1);
l_fstatfs:
  return sys_fstatfs(a0, a1);
l_statx:
  return sys_statx(a0, a1, a2, a3, a4);
l_fchmod:
  return sys_fchmod(a0, a1);
l_fchmodat:
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
  {
    struct vfs_node node;
    if (!cell_fd_stat((int)a0, &node)) { return -(int64_t)EBADF; }
    if (!node.is_dir) { return -(int64_t)ENOTDIR; }
    char path[128];
    if (!cell_fd_path((int)a0, path, sizeof(path))) { return -(int64_t)EBADF; }
    return cell_set_cwd(path) ? 0 : -(int64_t)ENAMETOOLONG;
  }
l_fsync:
  return 0;
l_getrandom:
  return sys_getrandom(a0, a1);
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

void handle_lower_sync(struct trap_frame *frame) {
  uint64_t ec = (frame->esr_el1 >> 26) & 0x3f;
  if (ec != EC_SVC_A64) {
    uint64_t far;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    uint64_t iss = frame->esr_el1 & 0x1ffffff;
    uint64_t dfsc = iss & 0x3f;
    bool write = (iss & (1u << 6)) != 0;
    if (ec == 0x20 && dfsc >= 0x04 && dfsc <= 0x07 && cell_handle_translation_fault(far, VMM_ACCESS_EXEC)) {
      return;
    }
    if (ec == 0x24 && dfsc >= 0x04 && dfsc <= 0x07 &&
        cell_handle_translation_fault(far, write ? VMM_ACCESS_WRITE : VMM_ACCESS_READ)) {
      return;
    }
    if (ec == 0x24 && write && dfsc >= 0x0c && dfsc <= 0x0f && cell_handle_cow_fault(far)) { return; }
    kprintf("[kernel] lower sync fault ec=%x dfsc=%x write=%u esr=%x elr=%p far=%p\n", (unsigned)ec, (unsigned)dfsc,
            write ? 1u : 0u, (unsigned)frame->esr_el1, (void *)(uintptr_t)frame->elr_el1, (void *)(uintptr_t)far);
    cell_dump_current_fault(frame->esr_el1, frame->elr_el1, far);
    cell_signal_current(11, frame);
    return;
  }
  int64_t ret = dispatch(frame);
  if (ret != SYSCALL_SWITCHED) { frame->x[0] = (uint64_t)ret; }
}
