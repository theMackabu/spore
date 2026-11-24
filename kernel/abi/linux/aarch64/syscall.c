#include "abi/linux/aarch64/syscall.h"
#include "abi/linux/shared/dispatch.h"
#include "arch/regs.h"
#include "proc/thread.h"
#include "proc/signal.h"

void linux_aarch64_syscall(struct trap_frame *frame) {
  int64_t result = linux_dispatch(frame, trap_syscall_number(frame), frame->x);
  if (result != CELL_SWITCHED) {
    trap_set_result(frame, (uint64_t)result);
    (void)cell_deliver_pending_signals_current(frame);
  }
}
