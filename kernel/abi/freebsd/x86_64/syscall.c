#include "abi/freebsd/dispatch.h"
#include "arch/regs.h"
#include "proc/signal.h"
#include "proc/thread.h"

void freebsd_result(struct trap_frame *f, int64_t rc) {
  if (rc < 0) {
    f->rax = freebsd_errno(-rc);
    f->rflags |= 1;
  } else {
    f->rax = rc;
    f->rdx = 0;
    f->rflags &= ~1ull;
  }
}

void freebsd_x86_64_syscall(struct trap_frame *f) {
  const uint64_t args[] = {f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9, 0};
  int64_t rc = freebsd_dispatch(f, f->rax, args);
  if (rc != CELL_SWITCHED) {
    freebsd_result(f, rc);
    (void)cell_deliver_pending_signals_current(f);
  }
}
