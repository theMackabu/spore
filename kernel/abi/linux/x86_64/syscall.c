#include "abi/linux/shared/dispatch.h"
#include "abi/linux/shared/fd/handlers.h"
#include "abi/linux/shared/numbers.h"
#include "abi/linux/shared/usercopy.h"
#include "arch/regs.h"
#include "mm/vmm.h"
#include "proc/domain.h"
#include "proc/policy.h"
#include "proc/signal.h"
#include "proc/thread.h"

static const uint16_t operations[] = {
  [309] = SYS_GETCPU,
  [0] = SYS_READ,
  [1] = SYS_WRITE,
  [3] = SYS_CLOSE,
  [5] = SYS_FSTAT,
  [8] = SYS_LSEEK,
  [9] = SYS_MMAP,
  [10] = SYS_MPROTECT,
  [11] = SYS_MUNMAP,
  [12] = SYS_BRK,
  [13] = SYS_RT_SIGACTION,
  [14] = SYS_RT_SIGPROCMASK,
  [15] = SYS_RT_SIGRETURN,
  [16] = SYS_IOCTL,
  [17] = SYS_PREAD64,
  [18] = SYS_PWRITE64,
  [19] = SYS_READV,
  [20] = SYS_WRITEV,
  [24] = SYS_SCHED_YIELD,
  [25] = SYS_MREMAP,
  [26] = SYS_MSYNC,
  [28] = SYS_MADVISE,
  [32] = SYS_DUP,
  [35] = SYS_NANOSLEEP,
  [38] = SYS_SETITIMER,
  [39] = SYS_GETPID,
  [41] = SYS_SOCKET,
  [42] = SYS_CONNECT,
  [43] = SYS_ACCEPT,
  [44] = SYS_SENDTO,
  [45] = SYS_RECVFROM,
  [46] = SYS_SENDMSG,
  [47] = SYS_RECVMSG,
  [48] = SYS_SHUTDOWN,
  [49] = SYS_BIND,
  [50] = SYS_LISTEN,
  [51] = SYS_GETSOCKNAME,
  [52] = SYS_GETPEERNAME,
  [53] = SYS_SOCKETPAIR,
  [54] = SYS_SETSOCKOPT,
  [55] = SYS_GETSOCKOPT,
  [56] = SYS_CLONE,
  [59] = SYS_EXECVE,
  [60] = SYS_EXIT,
  [61] = SYS_WAIT4,
  [62] = SYS_KILL,
  [63] = SYS_UNAME,
  [72] = SYS_FCNTL,
  [73] = SYS_FLOCK,
  [74] = SYS_FSYNC,
  [75] = SYS_FDATASYNC,
  [77] = SYS_FTRUNCATE,
  [79] = SYS_GETCWD,
  [80] = SYS_CHDIR,
  [81] = SYS_FCHDIR,
  [91] = SYS_FCHMOD,
  [93] = SYS_FCHOWN,
  [95] = SYS_UMASK,
  [96] = SYS_GETTIMEOFDAY,
  [97] = SYS_GETRLIMIT,
  [98] = SYS_GETRUSAGE,
  [99] = SYS_SYSINFO,
  [100] = SYS_TIMES,
  [102] = SYS_GETUID,
  [104] = SYS_GETGID,
  [105] = SYS_SETUID,
  [106] = SYS_SETGID,
  [107] = SYS_GETEUID,
  [108] = SYS_GETEGID,
  [109] = SYS_SET_PGID,
  [110] = SYS_GETPPID,
  [112] = SYS_SETSID,
  [115] = SYS_GETGROUPS,
  [116] = SYS_SETGROUPS,
  [117] = SYS_SETRESUID,
  [118] = SYS_GETRESUID,
  [119] = SYS_SETRESGID,
  [120] = SYS_GETRESGID,
  [121] = SYS_GET_PGID,
  [124] = SYS_GET_SID,
  [131] = SYS_SIGALTSTACK,
  [137] = SYS_STATFS,
  [138] = SYS_FSTATFS,
  [143] = SYS_SCHED_GETPARAM,
  [145] = SYS_SCHED_GETSCHEDULER,
  [157] = SYS_PRCTL,
  [160] = SYS_SETRLIMIT,
  [161] = SYS_CHROOT,
  [170] = SYS_SETHOSTNAME,
  [186] = SYS_GETTID,
  [200] = SYS_TKILL,
  [202] = SYS_FUTEX,
  [204] = SYS_SCHED_GETAFFINITY,
  [217] = SYS_GETDENTS64,
  [218] = SYS_SET_TID_ADDRESS,
  [228] = SYS_CLOCK_GETTIME,
  [229] = SYS_CLOCK_GETRES,
  [230] = SYS_CLOCK_NANOSLEEP,
  [231] = SYS_EXIT_GROUP,
  [233] = SYS_EPOLL_CTL,
  [234] = SYS_TGKILL,
  [254] = SYS_INOTIFY_ADD_WATCH,
  [255] = SYS_INOTIFY_RM_WATCH,
  [257] = SYS_OPENAT,
  [258] = SYS_MKDIRAT,
  [259] = SYS_MKNODAT,
  [260] = SYS_FCHOWNAT,
  [262] = SYS_NEWFSTATAT,
  [263] = SYS_UNLINKAT,
  [264] = SYS_RENAMEAT,
  [265] = SYS_LINKAT,
  [266] = SYS_SYMLINKAT,
  [267] = SYS_READLINKAT,
  [268] = SYS_FCHMODAT,
  [269] = SYS_FACCESSAT,
  [270] = SYS_PSELECT6,
  [271] = SYS_PPOLL,
  [273] = SYS_SET_ROBUST_LIST,
  [280] = SYS_UTIMENSAT,
  [281] = SYS_EPOLL_PWAIT,
  [288] = SYS_ACCEPT4,
  [290] = SYS_EVENTFD2,
  [291] = SYS_EPOLL_CREATE1,
  [292] = SYS_DUP3,
  [293] = SYS_PIPE2,
  [294] = SYS_INOTIFY_INIT1,
  [302] = SYS_PRLIMIT64,
  [316] = SYS_RENAMEAT2,
  [318] = SYS_GETRANDOM,
  [319] = SYS_MEMFD_CREATE,
  [324] = SYS_MEMBARRIER,
  [332] = SYS_STATX,
  [334] = SYS_RSEQ,
  [425] = SYS_IO_URING_SETUP,
  [426] = SYS_IO_URING_ENTER,
  [427] = SYS_IO_URING_REGISTER,
  [435] = SYS_CLONE3,
  [436] = SYS_CLOSE_RANGE,
  [439] = SYS_FACCESSAT2,
  [452] = SYS_FCHMODAT2,
};

void x86_set_fsbase(uint64_t value);
uint64_t x86_fsbase(void);

static void at_cwd(uint64_t args[6], unsigned count) {
  for (unsigned i = count; i; --i) {
    args[i] = args[i - 1];
  }
  args[0] = (uint64_t)-100;
}

void linux_x86_64_syscall(struct trap_frame *f) {
  uint64_t nr = f->rax;
  uint64_t args[] = {f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9};
  uint64_t op = nr < sizeof(operations) / sizeof(*operations) ? operations[nr] : 0;
  int64_t rc;
  if (nr == 7) {
    rc = cell_syscall_allowed(SYS_PPOLL) ? sys_poll_ms(f, args[0], args[1], (int)args[2]) : -1;
  } else if (nr == 158) {
    if (args[0] == 0x1002 && args[1] < USER_VA_LIMIT) {
      x86_set_fsbase(args[1]);
      rc = 0;
    } else if (args[0] == 0x1003) {
      uint64_t base = x86_fsbase();
      rc = syscall_user_writable(args[1], 8) && vmm_copy_to_user(syscall_active_as(), args[1], &base, 8) ? 0 : -14;
    } else {
      rc = -22;
    }
  } else {
    if (nr >= SYS_SPORE_SNAPSHOT && nr <= SYS_SPORE_NET_CONFIG) { op = nr; }
    if (op == SYS_CLONE) {
      args[3] = f->r8;
      args[4] = f->r10;
    }
    switch (nr) {
    case 2:
      op = SYS_OPENAT;
      at_cwd(args, 3);
      break;
    case 4:
    case 6:
      op = SYS_NEWFSTATAT;
      at_cwd(args, 2);
      args[3] = nr == 6 ? 0x100 : 0;
      break;
    case 21:
      op = SYS_FACCESSAT;
      at_cwd(args, 2);
      args[3] = 0;
      break;
    case 22:
      op = SYS_PIPE2;
      args[1] = 0;
      break;
    case 33:
      if (args[0] == args[1]) {
        rc = cell_syscall_allowed(SYS_DUP3) ? sys_fcntl(args[0], 1, 0) : -1;
        if (rc >= 0) { rc = (int)args[0]; }
        goto done;
      }
      op = SYS_DUP3;
      args[2] = 0;
      break;
    case 82:
    case 86:
      op = nr == 82 ? SYS_RENAMEAT : SYS_LINKAT;
      args[3] = args[1];
      args[2] = (uint64_t)-100;
      at_cwd(args, 1);
      args[4] = 0;
      break;
    case 83:
      op = SYS_MKDIRAT;
      at_cwd(args, 2);
      break;
    case 84:
    case 87:
      op = SYS_UNLINKAT;
      at_cwd(args, 1);
      args[2] = nr == 84 ? 0x200 : 0;
      break;
    case 85:
      op = SYS_OPENAT;
      args[3] = args[1];
      args[2] = 0100 | 01000 | 1;
      at_cwd(args, 1);
      break;
    case 88:
      op = SYS_SYMLINKAT;
      args[2] = args[1];
      args[1] = (uint64_t)-100;
      break;
    case 89:
      op = SYS_READLINKAT;
      at_cwd(args, 3);
      break;
    case 90:
      op = SYS_FCHMODAT;
      at_cwd(args, 2);
      args[3] = 0;
      break;
    case 92:
    case 94:
      op = SYS_FCHOWNAT;
      at_cwd(args, 3);
      args[4] = nr == 94 ? 0x100 : 0;
      break;
    case 133:
      op = SYS_MKNODAT;
      at_cwd(args, 3);
      break;
    case 213:
      if ((int)args[0] <= 0) {
        rc = -22;
        goto done;
      }
      op = SYS_EPOLL_CREATE1;
      args[0] = 0;
      break;
    case 253:
      op = SYS_INOTIFY_INIT1;
      args[0] = 0;
      break;
    case 284:
      op = SYS_EVENTFD2;
      args[1] = 0;
      break;
    case 57:
    case 58:
      op = SYS_CLONE;
      args[0] = nr == 57 ? 17 : 0x4111;
      args[1] = args[2] = args[3] = args[4] = 0;
      break;
    case 232:
      op = SYS_EPOLL_PWAIT;
      args[4] = args[5] = 0;
      break;
    }
    if (!op) {
      cell_note_unsupported_syscall(nr);
      rc = -38;
    } else {
      rc = linux_dispatch(f, op, args);
    }
  }
done:
  if (rc != CELL_SWITCHED) {
    f->rax = rc;
    (void)cell_deliver_pending_signals_current(f);
  }
}
