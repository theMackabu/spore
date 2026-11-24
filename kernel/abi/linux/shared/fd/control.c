#include "handlers.h"
#include "proc/fd.h"

enum {
  F_DUPFD = 0,
  F_GETFD = 1,
  F_SETFD = 2,
  F_DUPFD_CLOEXEC = 1030,
  F_ADD_SEALS = 1033,
  F_GET_SEALS = 1034,
  F_GETFL = 3,
  F_SETFL = 4,
  EINVAL = 22,
  EOPNOTSUPP = 95,
};

int64_t sys_fcntl(uint64_t fd_num, uint64_t cmd, uint64_t arg) {
  if (cmd == F_DUPFD) { return cell_fd_dup((int)fd_num, (int)arg); }
  if (cmd == F_DUPFD_CLOEXEC) {
    int fd = cell_fd_dup((int)fd_num, (int)arg);
    if (fd >= 0) {
      int rc = cell_fd_set_fd_flags(fd, 1);
      if (rc < 0) { return rc; }
    }
    return fd;
  }
  if (cmd == F_GETFD) { return cell_fd_get_fd_flags((int)fd_num); }
  if (cmd == F_SETFD) { return cell_fd_set_fd_flags((int)fd_num, (int)arg); }
  if (cmd == F_GETFL) { return cell_fd_get_flags((int)fd_num); }
  if (cmd == F_SETFL) { return cell_fd_set_flags((int)fd_num, (int)arg); }
  if (cmd == F_ADD_SEALS) { return cell_fd_add_seals((int)fd_num, (uint32_t)arg); }
  if (cmd == F_GET_SEALS) { return cell_fd_get_seals((int)fd_num); }
  return -(int64_t)EINVAL;
}

int64_t sys_close_range(uint64_t first, uint64_t end, uint64_t flags) {
  const uint64_t close_range_unshare = 1u << 1;
  const uint64_t close_range_cloexec = 1u << 2;
  if ((unsigned)first > (unsigned)end) { return -(int64_t)EINVAL; }
  if ((flags & ~(close_range_unshare | close_range_cloexec)) != 0) { return -(int64_t)EINVAL; }
  if ((flags & close_range_unshare) != 0) { return -(int64_t)EOPNOTSUPP; }
  cell_close_fd_range((unsigned)first, (unsigned)end, (flags & close_range_cloexec) != 0);
  return 0;
}
