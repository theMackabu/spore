#include "arch/bringup/user.h"
#include "arch/riscv64/syscall_frame.h"

void linux_riscv64_syscall(struct riscv64_syscall_frame *f) {
  switch (f->a[7]) {
  case 64: f->a[0] = user_write(f->a[0], f->a[1], f->a[2]); break;
  case 93:
  case 94: user_finish('E', f->a[0]);
  default: f->a[0] = -38; break;
  }
}
