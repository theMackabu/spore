#include "abi/linux/shared/open.h"
#include "proc/io.h"
#include "fs/notify.h"
#include "file_io.h"

#include "arch/regs.h"
#include "net/net.h"
#include "proc/domain.h"
#include "proc/user_namespace.h"
#include "proc/fd.h"
#include "proc/eventfd.h"
#include "proc/signalfd.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/pipe.h"
#include "fs/procfs.h"
#include "tty/pty.h"
#include "proc/signal.h"
#include "net/socket.h"
#include "proc/thread.h"
#include "tty/tty.h"
#include "fs/vfs.h"

#include <stddef.h>

enum {
  CELL_O_NONBLOCK = 04000,
  CELL_O_APPEND = 02000,
  IPPROTO_TCP = 6,
  EAGAIN = 11,
  EINVAL = 22,
  EINTR = 4,
  ENOTCONN = 107,
};

static int64_t write_device(struct open_file *file, struct domain *domain, uint64_t *offset, uint64_t buf, uint64_t len) {
  if (len == 0) { return 0; }
  switch (file->node.device) {
  case RAMFS_DEV_PROC_UID_MAP:
  case RAMFS_DEV_PROC_GID_MAP:
  case RAMFS_DEV_PROC_SETGROUPS: {
    char text[96];
    if (*offset || len >= sizeof(text)) { return -22; }
    if (!vmm_copy_from_user(cell_domain_as(domain), text, buf, len)) { return -14; }
    for (size_t i = 0; i < len; ++i) { if (!text[i]) { return -22; } }
    text[len] = 0;
    struct domain *target = cell_find_domain(file->node.proc.pid);
    if (!target) { return -3; }
    int rc = userns_write_map(domain, target->user_ns, file->node.device == RAMFS_DEV_PROC_GID_MAP,
                             file->node.device == RAMFS_DEV_PROC_SETGROUPS, text);
    if (rc) { return rc; }
    *offset += len;
    return (int64_t)len;
  }
  case RAMFS_DEV_NULL:
  case RAMFS_DEV_ZERO:
  case RAMFS_DEV_RANDOM:
  case RAMFS_DEV_URANDOM:
    return (int64_t)len;
  case RAMFS_DEV_FULL:
    return -28;
  case RAMFS_DEV_CONSOLE:
  case RAMFS_DEV_TTY:
    return cell_tty_write_console_from_user(domain, buf, len);
  case RAMFS_DEV_PTMX:
  case RAMFS_DEV_PTS:
    return -EINVAL;
  case RAMFS_DEV_BLK_ROOT:
    return cell_block_device_io(domain, file, offset, buf, len, true);
  default:
    return -EINVAL;
  }
}

static int64_t deliver_tty_signal_or_eintr(struct trap_frame *frame);

static int64_t read_device(struct open_file *file, struct domain *domain, uint64_t *offset, uint64_t buf, uint64_t len,
                           struct trap_frame *frame) {
  if (len == 0) { return 0; }
  switch (file->node.device) {
  case RAMFS_DEV_NULL:
    return 0;
  case RAMFS_DEV_ZERO:
  case RAMFS_DEV_FULL:
    return cell_read_generated(cell_domain_as(domain), buf, len, false);
  case RAMFS_DEV_RANDOM:
  case RAMFS_DEV_URANDOM:
    return cell_read_generated(cell_domain_as(domain), buf, len, true);
  case RAMFS_DEV_CONSOLE:
  case RAMFS_DEV_TTY: {
    int64_t n = cell_tty_read_to_user(domain, buf, len);
    if (n == -EINTR) { return deliver_tty_signal_or_eintr(frame); }
    if (n != 0) { return n; }
    if ((file->flags & CELL_O_NONBLOCK) != 0) { return -EAGAIN; }
    if (frame == NULL) { return 0; }
    return cell_block_current(frame, (struct thread_wait){.reason = WAIT_STDIN, .target = -1, .buf = buf, .len = len});
  }
  case RAMFS_DEV_PTMX:
  case RAMFS_DEV_PTS:
    return -EINVAL;
  case RAMFS_DEV_BLK_ROOT:
  case RAMFS_DEV_BLK_BOOT:
    return cell_block_device_io(domain, file, offset, buf, len, false);
  default: {
    int64_t got = cell_procfs_read_device(file, domain, buf, len, *offset);
    if (got > 0) { *offset += (uint64_t)got; }
    return got;
  }
  }
}

int cell_file_write_limit(struct domain *d,uint64_t offset,size_t *len) {
  if (!*len) {return 0;}
  if (offset>=d->fsize_soft) {cell_signal_domain(d,25,NULL);return -27;}
  if (*len>d->fsize_soft-offset) {*len=d->fsize_soft-offset;}
  return 0;
}

int64_t cell_fd_write(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = cell_file_at(domain, fd);
  if (file == NULL) { return -9; }
  if (file->type == OPEN_VFS && !linux_open_writable(file->flags)) { return -9; }
  if (file->type == OPEN_SOCKET && file->socket_proto != IPPROTO_TCP) { return cell_fd_udp_send(fd, 0, 0, buf, len); }
  if (!len && !(file->type == OPEN_UNIX_STREAM && cell_pipe_is_packet(file->unix_tx_pipe))) { return 0; }
  if (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP) {
    int64_t wrote = cell_socket_tcp_write_from_domain(domain, file, buf, len);
    if (wrote != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return wrote; }
    return cell_block_current_on_socket_write_timeout(fd, buf, len, file->so_sndtimeo_ticks, frame);
  }
  if (file->type == OPEN_EVENTFD) { return cell_eventfd_io(domain, file, buf, len, true, frame); }
  if (file->type == OPEN_INOTIFY) { return -22; }
  if (file->type == OPEN_PIPE) {
    int64_t wrote = cell_pipe_write_from_domain(domain, file, buf, len);
    if (wrote != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return wrote; }
    return cell_block_current_on_pipe(fd, buf, len, true, frame);
  }
  if (file->type == OPEN_PTY) {
    int64_t wrote = cell_pty_write_from_domain(domain, file, buf, len);
    if (wrote != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return wrote; }
    return cell_block_current_on_pty(fd, buf, len, true, frame);
  }
  if (file->type == OPEN_UNIX_STREAM) {
    if (!file->unix_connected) { return -ENOTCONN; }
    int64_t wrote = cell_pipe_write_user(domain, file->unix_tx_pipe, buf, len);
    if (wrote != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return wrote; }
    return cell_block_current_on_pipe(fd, buf, len, true, frame);
  }
  if (file->type != OPEN_STDOUT && file->type != OPEN_STDIN) {
    if (file->type != OPEN_VFS) { return -22; }
    return cell_file_regular_io(domain,file,buf,len,UINT64_MAX,true);
  }
  return cell_tty_write_console_from_user(domain, buf, len);
}

static int64_t deliver_tty_signal_or_eintr(struct trap_frame *frame) {
  int signal = cell_tty_take_pending_signal();
  if (signal == 0) { return -EINTR; }
  if (frame == NULL || cell_current_thread_internal() == NULL) { return -EINTR; }
  cell_current_thread_internal()->tf = *frame;
  trap_set_result(&cell_current_thread_internal()->tf, (uint64_t)(-(int64_t)EINTR));
  (void)cell_deliver_signal_to_thread(cell_current_thread_internal(), signal);
  *frame = cell_current_thread_internal()->tf;
  return CELL_SWITCHED;
}

int64_t cell_fd_read(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = cell_file_at(domain, fd);
  if (file == NULL) { return -9; }
  if (file->type == OPEN_SIGNALFD) { return cell_signalfd_read(cell_current_thread_internal(), file, buf, len, frame); }
  if (file->type == OPEN_EVENTFD) { return cell_eventfd_io(domain, file, buf, len, false, frame); }
  if (file->type == OPEN_INOTIFY) { return inotify_read(domain, file, buf, len, frame); }
  if (file->type == OPEN_PIPE) {
    int64_t got = cell_pipe_read_to_domain(domain, file, buf, len);
    if (got != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return got; }
    return cell_block_current_on_pipe(fd, buf, len, false, frame);
  }
  if (file->type == OPEN_PTY) {
    int64_t got = cell_pty_read_to_domain(domain, file, buf, len);
    if (got != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return got; }
    return cell_block_current_on_pty(fd, buf, len, false, frame);
  }
  if (file->type == OPEN_UNIX_STREAM) {
    if (!file->unix_connected) { return -ENOTCONN; }
    int64_t got = cell_pipe_read_user(domain, file->unix_rx_pipe, buf, len);
    if (got != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return got; }
    return cell_block_current_on_pipe(fd, buf, len, false, frame);
  }
  if (file->type == OPEN_SOCKET) {
    if (len == 0) { return 0; }
    return cell_fd_socket_recv(fd, buf, len, 0, false, frame, 0, 0);
  }
  if (file->type == OPEN_STDIN) {
    if (len == 0) { return 0; }
    int64_t n = cell_tty_read_to_user(domain, buf, len);
    if (n == -EINTR) { return deliver_tty_signal_or_eintr(frame); }
    if (n != 0) { return n; }
    if ((file->flags & CELL_O_NONBLOCK) != 0) { return -EAGAIN; }
    if (frame == NULL) { return 0; }
    return cell_block_current(frame, (struct thread_wait){.reason = WAIT_STDIN, .target = -1, .buf = buf, .len = len});
  }
  if (file->type == OPEN_VFS && !linux_open_readable(file->flags)) { return -9; }
  if (file->type != OPEN_VFS) { return -9; }
  if (file->node.is_dir) { return -21; }
  if (file->node.device != RAMFS_DEV_NONE) { return read_device(file, domain, &file->offset, buf, len, frame); }
  return cell_file_regular_io(domain,file,buf,len,UINT64_MAX,false);
}

int64_t cell_fd_positioned_io(int fd, uint64_t buf, uint64_t len, int64_t position, bool write) {
  if (position < 0) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = cell_file_at(domain, fd);
  if (!file) { return -9; }
  if ((file->type == OPEN_VFS || file->type == OPEN_PIPE) &&
      !(write ? linux_open_writable(file->flags) : linux_open_readable(file->flags))) { return -9; }
  if (file->type != OPEN_VFS || file->node.device == RAMFS_DEV_CONSOLE || file->node.device == RAMFS_DEV_TTY) { return -29; }
  return cell_file_regular_io(domain,file,buf,len,(uint64_t)position,write);
}

int64_t cell_file_regular_io(struct domain *domain, struct open_file *file, uint64_t buf, size_t len,
                             uint64_t position, bool write) {
  if (file->type != OPEN_VFS || file->node.is_dir) { return -21; }
  if (file->flags & LINUX_O_PATH || !(write ? linux_open_writable(file->flags) : linux_open_readable(file->flags))) { return -9; }
  if (!len) { return 0; }
  uint64_t *offset = position == UINT64_MAX ? &file->offset : &position;
  if (*offset > INT64_MAX) { return -EINVAL; }
  if (file->node.device != RAMFS_DEV_NONE) {
    return write ? write_device(file,domain,offset,buf,len) : read_device(file,domain,offset,buf,len,NULL);
  }
  if (len>INT64_MAX-*offset) {return -EINVAL;}
  uint64_t start=*offset;
  if (write) {
    if (file->flags & CELL_O_APPEND) {
      struct vfs_node fresh;
      if (!vfs_refresh(&file->node,&fresh)) {return -5;}
      start=fresh.size;
    }
    int rc=cell_file_write_limit(domain,start,&len);
    if (rc) {return rc;}
  }
  size_t done=0;
  while (done<len) {
    int64_t n=write ? cell_write_regular(domain,file,start+done,buf+done,len-done)
                    : cell_read_regular(domain,file,start+done,buf+done,len-done);
    if (n<=0) {return done ? (int64_t)done : n;}
    done+=n;
    *offset=start+done;
  }
  return done;
}
