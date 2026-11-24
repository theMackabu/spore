#include "abi/freebsd/console.h"
#include "arch/x86_64/syscall_frame.h"

void freebsd_x86_64_syscall(struct x86_64_syscall_frame *f) {
  int64_t rc = freebsd_console_syscall(f->rax, f->rdi, f->rsi, f->rdx, f->r10);
  if (rc < 0) {
    f->rax = -rc;
    f->rflags |= 1;
  } else {
    f->rax = rc;
    f->rdx = 0;
    f->rflags &= ~1ull;
  }
}
