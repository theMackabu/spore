#include "abi/linux/shared/dispatch.h"
#include "abi/linux/shared/numbers.h"
#include "abi/linux/shared/usercopy.h"
#include "arch/regs.h"
#include "mm/vmm.h"
#include "proc/domain.h"
#include "proc/signal.h"
#include "proc/thread.h"

void linux_riscv64_syscall(struct trap_frame *f) {
  int64_t rc;
  if (f->a[7] == 259) {
    __asm__ volatile("fence.i" ::: "memory");
    rc = 0;
  } else {
    rc = linux_dispatch(f, f->a[7], f->a);
  }
  if (rc != CELL_SWITCHED) {
    f->a[0] = rc;
    (void)cell_deliver_pending_signals_current(f);
  }
}
