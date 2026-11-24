#include "console.h"
#include "arch/bringup/user.h"

int64_t freebsd_console_syscall(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  if (nr == 0 || nr == 198) {
    nr = a;
    a = b;
    b = c;
    c = d;
  }
  switch (nr) {
  case 1: user_finish('E', a);
  case 4: return user_write(a, b, c);
  default: return -78;
  }
}
