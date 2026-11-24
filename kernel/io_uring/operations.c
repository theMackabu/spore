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
  return (unsigned)e->fd < r->nr_files ? r->files[e->fd].file : NULL;
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

int uring_issue(struct open_file *file, const struct sqe *e, uint32_t *flags) {
  if (!uring_opcode_supported(e->opcode) || e->personality ||
      (e->priority && !(e->opcode == OP_ACCEPT && e->priority == 1) &&
       !(e->opcode == OP_RECV && e->priority == 2 && (e->flags & BUFFER_SELECT))) ||
      (e->flags & ~(FIXED_FILE | IO_LINK | IO_HARDLINK | IO_DRAIN | IO_ASYNC | BUFFER_SELECT | CQE_SKIP_SUCCESS))) {
    return -EINVAL;
  }
  if ((e->flags & IO_ASYNC) && e->opcode != OP_NOP && e->opcode != OP_TIMEOUT_REMOVE && e->opcode != OP_READ &&
      e->opcode != OP_WRITE && e->opcode != OP_READV && e->opcode != OP_WRITEV) {
    return -95;
  }
  if (uring_ordered(&file->ring, e) && e->opcode != OP_NOP && e->opcode != OP_TIMEOUT_REMOVE && e->opcode != OP_READ &&
      e->opcode != OP_WRITE && e->opcode != OP_READV && e->opcode != OP_WRITEV && e->opcode != OP_POLL_ADD &&
      e->opcode != OP_TIMEOUT && e->opcode != OP_LINK_TIMEOUT) {
    return -95;
  }
  if (!cell_syscall_allowed(permissions[e->opcode])) { return -1; }
  if (e->file_index && e->opcode != OP_SEND && e->opcode != OP_CLOSE && e->opcode != OP_OPENAT &&
      e->opcode != OP_OPENAT2 && e->opcode != OP_SOCKET && e->opcode != OP_MSG_RING) {
    return -95;
  }
  if (e->flags & BUFFER_SELECT) {
    if ((e->opcode != OP_READ && e->opcode != OP_RECV) || e->addr || (e->flags & IO_ASYNC)) { return -EINVAL; }
    struct open_file *target = uring_target(&file->ring, e);
    if (!target) { return -EBADF; }
    if (e->opcode == OP_RECV && target->type != OPEN_SOCKET && target->type != OPEN_UNIX_STREAM) { return -88; }
    if (e->opcode == OP_READ && e->off != UINT64_MAX && !cell_syscall_allowed(SYS_PREAD64)) { return -1; }
    if (e->opcode == OP_READ && e->op_flags & ~8u) { return -95; }
    int rc = (e->priority == 2 || uring_ordered(&file->ring, e))
               ? -11
               : uring_selected_io(&file->ring, cell_current_domain_internal(), target, e, flags);
    if (rc == -11 && !(e->op_flags & (e->opcode == OP_READ ? 8u : 64u))) { return uring_defer_io(file, target, e); }
    return rc;
  }
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
  case OP_LINK_TIMEOUT:
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
    if (e->op_flags & ~1u) { return -EINVAL; }
    if ((e->flags & IO_ASYNC) || uring_ordered(&file->ring, e)) { return uring_defer_io(file, NULL, e); }
    return e->op_flags ? (int)e->len : 0;
  case OP_SYNC_FILE_RANGE:
    if ((e->op_flags & ~7u) || e->off > INT64_MAX || e->len > INT64_MAX - e->off) { return -EINVAL; }
    return sys_fsync(e->fd);
  case OP_FSYNC:
    return (e->op_flags & ~1u) ? -EINVAL : sys_fsync(e->fd);
  case OP_OPENAT:
    return uring_direct_result(&file->ring, e->file_index, sys_openat(NULL, e->fd, e->addr, e->op_flags, e->len));
  case OP_OPENAT2:
    return uring_direct_result(&file->ring, e->file_index, openat2(e));
  case OP_CLOSE:
    if (e->file_index) { return uring_close_direct(&file->ring, e->file_index); }
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
    return e->op_flags ? -EINVAL : uring_direct_result(&file->ring, e->file_index, sys_socket(e->fd, e->off, e->len));
  case OP_BIND:
    return sys_bind(e->fd, e->addr, e->off);
  case OP_LISTEN:
    return sys_listen(e->fd, e->len);
  case OP_SHUTDOWN:
    return sys_shutdown(e->fd, e->len);
  case OP_PROVIDE_BUFFERS:
  case OP_REMOVE_BUFFERS:
    return uring_group_op(&file->ring, e);
  case OP_FILES_UPDATE:
    if (e->off > UINT32_MAX || !file->ring.files) { return -EINVAL; }
    return uring_update_files(&file->ring, e->off, e->addr, e->len, 0, true);
  case OP_MSG_RING:
    return uring_message(file, e);
  case OP_PIPE:
    return cell_fd_pipe2(e->addr, e->op_flags);
  case OP_SENDMSG:
  case OP_RECVMSG:
  case OP_CONNECT:
  case OP_ACCEPT:
  case OP_SEND:
  case OP_RECV:
    return uring_socket_op(file, e);
  default:
    return -EINVAL;
  }
}
