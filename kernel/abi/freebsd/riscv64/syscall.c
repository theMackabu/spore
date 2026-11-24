#include "abi/freebsd/dispatch.h"
#include "arch/regs.h"
#include "proc/signal.h"
#include "proc/thread.h"

void freebsd_result(struct trap_frame *f, int64_t rc) {
  f->t[0] = rc < 0;
  f->a[0] = rc < 0 ? freebsd_errno(-rc) : rc;
  if (rc >= 0) { f->a[1] = 0; }
}

void freebsd_riscv64_syscall(struct trap_frame *f) {
  int64_t rc = freebsd_dispatch(f, f->t[0], f->a);
  if (rc != CELL_SWITCHED) {
    freebsd_result(f, rc);
    (void)cell_deliver_pending_signals_current(f);
  }
}
