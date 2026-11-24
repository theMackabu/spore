#include "../usercopy.h"
#include "handlers.h"
#include "proc/io.h"

enum { EFAULT = 14, EINVAL = 22 };

int64_t sys_getrandom(uint64_t buf, uint64_t len, uint64_t flags) {
  enum { NONBLOCK = 1, RANDOM = 2, INSECURE = 4 };
  if ((flags & ~(uint64_t)(NONBLOCK | RANDOM | INSECURE)) || (flags & (RANDOM | INSECURE)) == (RANDOM | INSECURE)) { return -EINVAL; }
  if (!syscall_user_writable(buf, len)) { return -EFAULT; }
  const struct user_address_space *as = syscall_active_as();
  uint64_t done = 0;
  while (done < len) {
    int64_t n = cell_read_generated(as, buf + done, len - done, true);
    if (n < 0) { return done ? (int64_t)done : n; }
    done += (uint64_t)n;
  }
  return (int64_t)done;
}
