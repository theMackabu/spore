#include "abi/linux/aarch64/syscall.h"
#include "abi/linux/shared/dispatch.h"
#include "arch/regs.h"
#include "proc/seccomp.h"
#include "proc/signal.h"
#include "proc/thread.h"

void linux_aarch64_syscall(struct trap_frame *frame) {
  uint64_t nr = trap_syscall_number(frame);
  int64_t result;
  if (seccomp_check(frame, nr, frame->x, &result)) { result = linux_dispatch(frame, nr, frame->x); }
  if (result != CELL_SWITCHED) {
    trap_set_result(frame, (uint64_t)result);
    (void)cell_deliver_pending_signals_current(frame);
  }
}
