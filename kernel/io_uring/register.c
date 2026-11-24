#include "internal.h"
#include "abi/linux/shared/usercopy.h"

int64_t sys_io_uring_register(int fd, uint32_t op, uint64_t addr, uint32_t count) {
  if (!uring_file(fd)) { return -EBADF; }
  if (op != REGISTER_PROBE || count > 256) { return -EINVAL; }
  struct probe_op {
    uint8_t op, pad;
    uint16_t flags;
    uint32_t reserved;
  };
  struct {
    uint8_t last, len;
    uint8_t pad[14];
    struct probe_op ops[OP_EPOLL_CTL + 1];
  } probe = {0};
  probe.last = OP_EPOLL_CTL;
  probe.len = count < OP_EPOLL_CTL + 1 ? count : OP_EPOLL_CTL + 1;
  for (unsigned i = 0; i < probe.len; ++i) {
    probe.ops[i].op = i;
    probe.ops[i].flags = i == OP_NOP || i == OP_EPOLL_CTL;
  }
  size_t len = 16 + probe.len * sizeof(struct probe_op);
  return syscall_user_writable(addr, len) && vmm_copy_to_user(syscall_active_as(), addr, &probe, len) ? 0 : -EFAULT;
}
