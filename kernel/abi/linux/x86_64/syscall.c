#include "arch/bringup/user.h"
#include "abi/linux/shared/fd/write.h"
#include "arch/x86_64/syscall_frame.h"

void linux_x86_64_syscall(struct x86_64_syscall_frame *f) {
  switch (f->rax) {
  case 1: f->rax = sys_write(NULL, f->rdi, f->rsi, f->rdx); break;
  case 60:
  case 231: user_finish('E', f->rdi);
  default: f->rax = -38; break;
  }
}
