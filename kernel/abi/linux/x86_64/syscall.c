#include "abi/linux/shared/dispatch.h"
#include "abi/linux/shared/fd/handlers.h"
#include "abi/linux/shared/numbers.h"
#include "abi/linux/shared/usercopy.h"
#include "arch/regs.h"
#include "mm/vmm.h"
#include "proc/domain.h"
#include "proc/policy.h"
#include "proc/seccomp.h"
#include "proc/signal.h"
#include "proc/thread.h"

static const uint16_t operations[] = {
#define LINUX_SYSCALL(name, generic, x86_64) [x86_64] = SYS_##name,
#include "../syscalls.def"
#undef LINUX_SYSCALL
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
  if (!seccomp_check(f, nr, args, &rc)) { goto done; }
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
