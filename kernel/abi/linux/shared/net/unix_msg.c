#include "abi/linux/shared/usercopy.h"
#include "message_abi.h"
#include "abi/linux/shared/message.h"
#include "abi/linux/shared/unix.h"
#include "handlers.h"
#include "net/unix.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "proc/pipe.h"
#include "proc/signal.h"
#include "proc/thread.h"

enum { EAGAIN = 11, MSG_PEEK = 2, MSG_CTRUNC = 8, MSG_CMSG_CLOEXEC = 0x40000000, SCM_RIGHTS = 1 };

struct cmsghdr64 {
  uint64_t len;
  int32_t level, type;
};

static size_t cmsg_align(size_t len) { return (len + 7) & ~(size_t)7; }

static int collect_rights(struct domain *domain, const struct msghdr64 *msg, struct unix_rights **out) {
  if (!msg->controllen) { return 0; }
  if (!msg->control || !syscall_user_readable(msg->control, msg->controllen)) { return -EFAULT; }
  struct user_address_space *as = cell_domain_as(domain);
  unsigned count = 0;
  int32_t fds[UNIX_MAX_RIGHTS];
  for (size_t off = 0; off + sizeof(struct cmsghdr64) <= msg->controllen;) {
    struct cmsghdr64 cmsg;
    if (!vmm_copy_from_user(as, &cmsg, msg->control + off, sizeof(cmsg))) { return -EFAULT; }
    if (cmsg.len < sizeof(cmsg) || cmsg.len > msg->controllen - off) { return -EINVAL; }
    if (cmsg.level == SOL_SOCKET && cmsg.type == SCM_RIGHTS) {
      size_t n = (cmsg.len - sizeof(cmsg)) / sizeof(*fds);
      if (n > UNIX_MAX_RIGHTS - count) { return -EINVAL; }
      if (!vmm_copy_from_user(as, fds + count, msg->control + off + sizeof(cmsg), n * sizeof(*fds))) { return -EFAULT; }
      count += (unsigned)n;
    }
    off += cmsg_align(cmsg.len);
  }
  return unix_prepare_rights(domain, fds, count, out);
}

static int write_control(struct domain *domain, const struct unix_rights *rights, const struct msghdr64 *msg,
                         uint64_t addr, uint32_t flags) {
  struct {
    struct cmsghdr64 header;
    int fds[UNIX_MAX_RIGHTS];
  } control = {0};
  unsigned count = 0;
  uint64_t used = 0;
  int32_t result_flags = 0;
  struct user_address_space *as = cell_domain_as(domain);
  if (rights) {
    unsigned capacity = 0;
    if (msg->control && msg->controllen > sizeof(control.header)) {
      capacity = (msg->controllen - sizeof(control.header)) / sizeof(int);
      if (capacity > UNIX_MAX_RIGHTS) { capacity = UNIX_MAX_RIGHTS; }
    }
    bool truncated;
    count = unix_install_rights(domain, rights, control.fds, capacity, flags & MSG_CMSG_CLOEXEC, &truncated);
    result_flags = truncated ? MSG_CTRUNC : 0;
    if (count) {
      control.header = (struct cmsghdr64){sizeof(control.header) + count * sizeof(int), SOL_SOCKET, SCM_RIGHTS};
      used = cmsg_align(control.header.len);
      if (used > msg->controllen) { used = msg->controllen; }
      if (!cell_domain_ensure_user_range(domain, msg->control, used, VMM_ACCESS_WRITE) ||
          !vmm_copy_to_user(as, msg->control, &control, used)) { goto fault; }
    }
  }
  if (vmm_copy_to_user(as, addr + offsetof(struct msghdr64, controllen), &used, sizeof(used)) &&
      vmm_copy_to_user(as, addr + offsetof(struct msghdr64, flags), &result_flags, sizeof(result_flags))) { return 0; }
fault:
  while (count) { cell_close_domain_fd(domain, control.fds[--count]); }
  return -EFAULT;
}

int64_t linux_unix_transfer(struct domain *domain, struct open_file *file, uint64_t msg_addr, uint32_t flags,
                              bool write, struct unix_rights **pending) {
  struct msghdr64 msg;
  if (!vmm_copy_from_user(cell_domain_as(domain), &msg, msg_addr, sizeof(msg))) { return -EFAULT; }
  struct linux_iov_cursor cursor;
  int rc = linux_iov_init(&cursor, domain, msg.iov, msg.iovlen);
  if (rc) { return rc; }
  if (write) { return cell_pipe_write_data(file->unix_tx_pipe, cursor.capacity, linux_gather_iov, &cursor, pending); }

  struct pipe_obj *pipe = file->unix_rx_pipe;
  struct unix_rights_queue *queue = cell_pipe_rights(pipe);
  uint64_t start = cell_pipe_read_offset(pipe), bytes = cursor.capacity;
  unix_next_rights(queue, start, &bytes);
  int64_t n = cell_pipe_peek_data(pipe, bytes, linux_scatter_iov, &cursor);
  if (n < 0) { return n; }
  bytes = (uint64_t)n;
  const struct unix_rights *rights = unix_next_rights(queue, start, &bytes);
  rc = write_control(domain, rights, &msg, msg_addr, flags);
  if (rc) { return rc; }
  if (n && !(flags & MSG_PEEK)) { cell_pipe_consume(pipe, (uint32_t)n); }
  return n;
}

int64_t linux_unix_message(struct trap_frame *frame, int fd, uint64_t addr, const struct msghdr64 *msg,
                             uint32_t flags, bool write) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = cell_fd_file(fd);
  if (!file || file->type != OPEN_UNIX_STREAM || !file->unix_connected) { return -ENOTCONN; }
  struct unix_rights *rights = NULL;
  if (write) {
    int rc = collect_rights(domain, msg, &rights);
    if (rc) { return rc; }
  }
  int64_t rc = linux_unix_transfer(domain, file, addr, flags, write, &rights);
  if (rc == -EAGAIN && frame && !(flags & MSG_DONTWAIT) && !(file->flags & 04000)) {
    struct thread_wait wait = {.reason = WAIT_PIPE, .target = fd, .socket_msg = true, .write = write,
                               .socket_flags = flags, .socket_msg_addr = addr, .unix_rights = rights};
    uint64_t timeout = write ? file->so_sndtimeo_ticks : file->so_rcvtimeo_ticks;
    if (timeout) { wait.has_deadline = true; wait.deadline_tick = cell_uptime_ticks() + timeout; }
    return cell_block_current(frame, wait);
  }
  unix_release_rights(rights);
  if (rc == -EPIPE && !(flags & MSG_NOSIGNAL) && frame && cell_signal_current(SIGPIPE, frame)) { return CELL_SWITCHED; }
  return rc;
}
