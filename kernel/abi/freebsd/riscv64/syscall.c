#include "abi/freebsd/console.h"
#include "arch/riscv64/syscall_frame.h"

void freebsd_riscv64_syscall(struct riscv64_syscall_frame *f) {
  int64_t rc = freebsd_console_syscall(f->t[0], f->a[0], f->a[1], f->a[2], f->a[3]);
  f->t[0] = rc < 0;
  f->a[0] = rc < 0 ? -rc : rc;
  if (rc >= 0) { f->a[1] = 0; }
}
