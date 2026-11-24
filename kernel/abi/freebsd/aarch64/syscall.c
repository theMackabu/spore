#include "abi/freebsd/aarch64.h"
#include "abi/freebsd/dispatch.h"
#include "arch/regs.h"
#include "proc/signal.h"
#include "proc/thread.h"

void freebsd_result(struct trap_frame *frame, int64_t result) {
  constexpr uint64_t carry = 1ull << 29;
  if (result < 0) {
    frame->x[0] = freebsd_errno((unsigned)-result);
    frame->spsr_el1 |= carry;
  } else {
    frame->x[0] = result;
    frame->x[1] = 0;
    frame->spsr_el1 &= ~carry;
  }
}

void freebsd_aarch64_syscall(struct trap_frame *frame) {
  int64_t result = freebsd_dispatch(frame, trap_syscall_number(frame), frame->x);
  if (result != CELL_SWITCHED) {
    freebsd_result(frame, result);
    (void)cell_deliver_pending_signals_current(frame);
  }
}
