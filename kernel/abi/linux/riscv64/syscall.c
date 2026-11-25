#include "abi/linux/shared/dispatch.h"
#include "arch/regs.h"
#include "proc/policy.h"
#include "proc/signal.h"
#include "proc/seccomp.h"
#include "proc/thread.h"

int riscv_flush_icache(uint64_t flags);

void linux_riscv64_syscall(struct trap_frame *f) {
  int64_t rc;
  if (!seccomp_check(f, f->a[7], f->a, &rc)) { goto done; }
  if (f->a[7] == 259) {
    rc = cell_syscall_allowed(259) ? riscv_flush_icache(f->a[2]) : -1;
  } else {
    rc = linux_dispatch(f, f->a[7], f->a);
  }
done:
  if (rc != CELL_SWITCHED) {
    f->a[0] = rc;
    (void)cell_deliver_pending_signals_current(f);
  }
}
