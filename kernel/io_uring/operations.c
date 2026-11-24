#include "abi/linux/shared/fd/handlers.h"
#include "abi/linux/shared/fs/handlers.h"
#include "abi/linux/shared/mm/handlers.h"
#include "abi/linux/shared/net/handlers.h"
#include "abi/linux/shared/numbers.h"
#include "abi/linux/shared/usercopy.h"
#include "internal.h"
#include "proc/domain.h"
#include "proc/policy.h"

static const uint16_t permissions[OP_LAST + 1] = {
#define URING_OP(name, number, syscall) [number] = SYS_##syscall,
#include "operations.def"
#undef URING_OP
};

bool uring_opcode_supported(unsigned op) {
  return op <= OP_LAST && permissions[op];
}

struct open_file *uring_target(struct uring *r, const struct sqe *e) {
  if (!(e->flags & FIXED_FILE)) { return cell_fd_file(e->fd); }
  return (unsigned)e->fd < r->nr_files ? r->files[e->fd] : NULL;
}

static int openat2(const struct sqe *e) {
  struct {
    uint64_t flags, mode, resolve;
  } how;
  if (e->len != sizeof(how)) { return -EINVAL; }
  if (!syscall_user_readable(e->off, sizeof(how)) ||
      !vmm_copy_from_user(syscall_active_as(), &how, e->off, sizeof(how))) {
    return -EFAULT;
  }
  if (how.resolve || how.flags >> 32 || how.mode & ~07777ull) { return -EINVAL; }
  return sys_openat(NULL, e->fd, e->addr, how.flags, how.mode);
}

int uring_issue(struct open_file *file, const struct sqe *e) {
  if (!uring_opcode_supported(e->opcode) || e->personality || e->priority ||
      (e->flags & ~(FIXED_FILE | IO_LINK | IO_HARDLINK | IO_DRAIN | CQE_SKIP_SUCCESS))) {
    return -EINVAL;
  }
  if (!cell_syscall_allowed(permissions[e->opcode])) { return -1; }
  if (e->file_index && e->opcode != OP_SEND && e->opcode != OP_CLOSE) { return -95; }
  switch (e->opcode) {
  case OP_READ_FIXED:
  case OP_WRITE_FIXED:
    return uring_fixed_io(file, e);
  case OP_READ:
  case OP_WRITE:
  case OP_READV:
  case OP_WRITEV:
    return uring_rw(file, e);
  case OP_POLL_ADD:
  case OP_POLL_REMOVE:
  case OP_ASYNC_CANCEL:
  case OP_TIMEOUT:
  case OP_TIMEOUT_REMOVE:
    return uring_wait_op(file, e);
  case OP_FIXED_FD_INSTALL: {
    if (!(e->flags & FIXED_FILE) || e->op_flags & ~1u) { return -EINVAL; }
    struct open_file *target = uring_target(&file->ring, e);
    if (!target) { return -EBADF; }
    cell_retain_open_file(target);
    return cell_install_fd(cell_current_domain_internal(), target, !e->op_flags);
  }
  }
  if (e->flags & FIXED_FILE) { return -95; }
  switch (e->opcode) {
  case OP_NOP:
    return e->op_flags ? -EINVAL : 0;
  case OP_FSYNC:
    return (e->op_flags & ~1u) ? -EINVAL : sys_fsync(e->fd);
  case OP_OPENAT:
    return sys_openat(NULL, e->fd, e->addr, e->op_flags, e->len);
  case OP_OPENAT2:
    return openat2(e);
  case OP_CLOSE:
    if (e->file_index) { return -95; }
    if (uring_file(e->fd)) { return -EBADF; }
    return cell_fd_close(e->fd);
  case OP_STATX:
    return sys_statx(e->fd, e->addr, e->op_flags, e->len, e->off);
  case OP_FTRUNCATE:
    return sys_ftruncate(e->fd, e->off);
  case OP_MADVISE:
    return sys_madvise(e->addr, e->off ? e->off : e->len, e->op_flags);
  case OP_EPOLL_CTL:
    return sys_epoll_ctl(e->fd, e->len, e->off, e->addr);
  case OP_RENAMEAT:
    return sys_renameat(e->fd, e->addr, (int)e->len, e->off, e->op_flags);
  case OP_LINKAT:
    return sys_linkat(e->fd, e->addr, (int)e->len, e->off, e->op_flags);
  case OP_UNLINKAT:
    return sys_unlinkat(e->fd, e->addr, e->op_flags);
  case OP_MKDIRAT:
    return sys_mkdirat(e->fd, e->addr, e->len);
  case OP_SYMLINKAT:
    return sys_symlinkat(e->addr, e->fd, e->off);
  case OP_SOCKET:
    return e->op_flags ? -EINVAL : sys_socket(e->fd, e->off, e->len);
  case OP_BIND:
    return sys_bind(e->fd, e->addr, e->off);
  case OP_LISTEN:
    return sys_listen(e->fd, e->len);
  case OP_SHUTDOWN:
    return sys_shutdown(e->fd, e->len);
  case OP_PIPE:
    return cell_fd_pipe2(e->addr, e->op_flags);
  case OP_SENDMSG:
    return sys_sendmsg(NULL, e->fd, e->addr, e->op_flags);
  case OP_RECVMSG:
    return sys_recvmsg(NULL, e->fd, e->addr, e->op_flags);
  case OP_CONNECT:
    return sys_connect(NULL, e->fd, e->addr, e->off);
  case OP_ACCEPT:
    return sys_accept4(NULL, e->fd, e->addr, e->off, e->op_flags);
  case OP_SEND:
    return sys_sendto(NULL, e->fd, e->addr, e->len, e->op_flags, e->off, e->file_index);
  case OP_RECV:
    return sys_recvfrom(NULL, e->fd, e->addr, e->len, e->op_flags, 0, 0);
  default:
    return -EINVAL;
  }
}
