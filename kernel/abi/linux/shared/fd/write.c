#include "write.h"
#include "proc/io.h"

int64_t sys_write(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len) {
  int error = linux_check_write_buffer(fd, buf, len);
  if (error) { return error; }
  return cell_fd_write((int)fd, buf, len, frame);
}
