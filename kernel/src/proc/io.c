#include "proc/io.h"

#include "cell.h"
#include "mem.h"
#include "net.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/pipe.h"
#include "proc/procfs.h"
#include "proc/signal.h"
#include "proc/socket.h"
#include "proc/thread.h"
#include "proc/tty.h"
#include "random.h"
#include "vfs.h"
#include "virtio_blk.h"

#include <stddef.h>

enum {
  FILE_IO_CHUNK = 128 * 1024,
  CELL_O_ACCMODE = 3,
  CELL_O_WRONLY = 1,
  CELL_O_RDWR = 2,
  CELL_O_NONBLOCK = 04000,
  CELL_O_APPEND = 02000,
  IPPROTO_TCP = 6,
  EAGAIN = 11,
  EFAULT = 14,
  EINVAL = 22,
  EINTR = 4,
};

/*
 * The big kernel lock serializes synchronous VFS/device copies, so one shared
 * scratch page avoids placing a 4 KiB buffer on every exception stack.
 */
static uint8_t file_io_tmp[FILE_IO_CHUNK];

static struct domain *current_domain(void) {
  return cell_current_domain_internal();
}

static int64_t write_console_from_user(struct domain *domain, uint64_t buf, uint64_t len) {
  return cell_tty_write_console_from_user(domain, buf, len);
}

static int64_t write_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len) {
  switch (file->node.device) {
  case RAMFS_DEV_NULL:
  case RAMFS_DEV_ZERO:
  case RAMFS_DEV_RANDOM:
  case RAMFS_DEV_URANDOM:
    return (int64_t)len;
  case RAMFS_DEV_FULL:
    return -28;
  case RAMFS_DEV_CONSOLE:
  case RAMFS_DEV_TTY:
    return write_console_from_user(domain, buf, len);
  case RAMFS_DEV_PROCINFO:
  case RAMFS_DEV_MEMINFO:
  case RAMFS_DEV_CPUINFO:
  case RAMFS_DEV_UPTIME:
  case RAMFS_DEV_LOADAVG:
  case RAMFS_DEV_MOUNTS:
  case RAMFS_DEV_STAT:
  case RAMFS_DEV_NET_DEV:
  case RAMFS_DEV_KMSG:
  case RAMFS_DEV_FILESYSTEMS:
  case RAMFS_DEV_PARTITIONS:
  case RAMFS_DEV_DEVICES:
  case RAMFS_DEV_FSSTATS:
  case RAMFS_DEV_PROC_PID_STAT:
  case RAMFS_DEV_PROC_PID_STATUS:
  case RAMFS_DEV_PROC_PID_CMDLINE:
  case RAMFS_DEV_PROC_PID_STATM:
  case RAMFS_DEV_PROC_PID_COMM:
  case RAMFS_DEV_PROC_PID_MOUNTS:
  case RAMFS_DEV_PROC_PID_CWD:
  case RAMFS_DEV_PROC_PID_EXE:
  case RAMFS_DEV_FS_ROOT:
  case RAMFS_DEV_FS_BOOT:
  case RAMFS_DEV_FS_RAM0:
  case RAMFS_DEV_FS_TMP:
  case RAMFS_DEV_SYS_CPU_POSSIBLE:
  case RAMFS_DEV_SYS_CPU_PRESENT:
  case RAMFS_DEV_SYS_CPU_ONLINE:
  case RAMFS_DEV_SYS_CPU_CORE_ONLINE:
  case RAMFS_DEV_BLK_BOOT:
    return -22;
  case RAMFS_DEV_BLK_ROOT: {
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > FILE_IO_CHUNK) { chunk = FILE_IO_CHUNK; }
      if (!vmm_copy_from_user(cell_domain_as(domain), file_io_tmp, buf + done, (size_t)chunk)) { return -14; }
      if (!virtio_blk_write(file->offset, file_io_tmp, (uint32_t)chunk)) { return -5; }
      file->offset += chunk;
      done += chunk;
    }
    return (int64_t)done;
  }
  case RAMFS_DEV_NONE:
    break;
  }
  return -22;
}

static int64_t deliver_tty_signal_or_eintr(struct trap_frame *frame);

static int64_t read_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len,
                           struct trap_frame *frame) {
  if (len == 0) { return 0; }
  switch (file->node.device) {
  case RAMFS_DEV_NULL:
    return 0;
  case RAMFS_DEV_ZERO:
  case RAMFS_DEV_FULL:
  case RAMFS_DEV_RANDOM:
  case RAMFS_DEV_URANDOM: {
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > FILE_IO_CHUNK) { chunk = FILE_IO_CHUNK; }
      if (file->node.device == RAMFS_DEV_RANDOM || file->node.device == RAMFS_DEV_URANDOM) {
        random_bytes(file_io_tmp, (size_t)chunk);
      } else {
        kmemset(file_io_tmp, 0, (size_t)chunk);
      }
      if (!vmm_copy_to_user(cell_domain_as(domain), buf + done, file_io_tmp, (size_t)chunk)) {
        kmemset(file_io_tmp, 0, FILE_IO_CHUNK);
        return -14;
      }
      done += chunk;
    }
    kmemset(file_io_tmp, 0, FILE_IO_CHUNK);
    return (int64_t)len;
  }
  case RAMFS_DEV_CONSOLE:
  case RAMFS_DEV_TTY: {
    int64_t n = cell_tty_read_to_user(domain, buf, len);
    if (n == -EINTR) { return deliver_tty_signal_or_eintr(frame); }
    if (n != 0) { return n; }
    if ((file->flags & CELL_O_NONBLOCK) != 0) { return -EAGAIN; }
    if (frame == NULL) { return 0; }
    cell_save_current(frame);
    cell_current_thread_internal()->state = THREAD_BLOCKED;
    cell_current_thread_internal()->running_cpu = -1;
    cell_current_thread_internal()->wait_reason = WAIT_STDIN;
    cell_current_thread_internal()->stdin_buf = buf;
    cell_current_thread_internal()->stdin_len = len;
    cell_schedule(frame);
    return CELL_SWITCHED;
  }
  case RAMFS_DEV_PROCINFO:
  case RAMFS_DEV_MEMINFO:
  case RAMFS_DEV_CPUINFO:
  case RAMFS_DEV_UPTIME:
  case RAMFS_DEV_LOADAVG:
  case RAMFS_DEV_MOUNTS:
  case RAMFS_DEV_STAT:
  case RAMFS_DEV_NET_DEV:
  case RAMFS_DEV_KMSG:
  case RAMFS_DEV_FILESYSTEMS:
  case RAMFS_DEV_PARTITIONS:
  case RAMFS_DEV_DEVICES:
  case RAMFS_DEV_FSSTATS:
  case RAMFS_DEV_PROC_PID_STAT:
  case RAMFS_DEV_PROC_PID_STATUS:
  case RAMFS_DEV_PROC_PID_CMDLINE:
  case RAMFS_DEV_PROC_PID_STATM:
  case RAMFS_DEV_PROC_PID_COMM:
  case RAMFS_DEV_PROC_PID_MOUNTS:
  case RAMFS_DEV_PROC_PID_CWD:
  case RAMFS_DEV_PROC_PID_EXE:
  case RAMFS_DEV_FS_ROOT:
  case RAMFS_DEV_FS_BOOT:
  case RAMFS_DEV_FS_RAM0:
  case RAMFS_DEV_FS_TMP:
  case RAMFS_DEV_SYS_CPU_POSSIBLE:
  case RAMFS_DEV_SYS_CPU_PRESENT:
  case RAMFS_DEV_SYS_CPU_ONLINE:
  case RAMFS_DEV_SYS_CPU_CORE_ONLINE:
    return cell_procfs_read_device(file, domain, buf, len);
  case RAMFS_DEV_BLK_ROOT: {
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > FILE_IO_CHUNK) { chunk = FILE_IO_CHUNK; }
      if (!virtio_blk_read(file->offset, file_io_tmp, (uint32_t)chunk)) { return done == 0 ? -5 : (int64_t)done; }
      if (!vmm_copy_to_user(cell_domain_as(domain), buf + done, file_io_tmp, (size_t)chunk)) { return -14; }
      file->offset += chunk;
      done += chunk;
    }
    return (int64_t)done;
  }
  case RAMFS_DEV_BLK_BOOT: {
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > FILE_IO_CHUNK) { chunk = FILE_IO_CHUNK; }
      if (!virtio_blk_read_boot(file->offset, file_io_tmp, (uint32_t)chunk)) { return done == 0 ? -5 : (int64_t)done; }
      if (!vmm_copy_to_user(cell_domain_as(domain), buf + done, file_io_tmp, (size_t)chunk)) { return -14; }
      file->offset += chunk;
      done += chunk;
    }
    return (int64_t)done;
  }
  case RAMFS_DEV_NONE:
    break;
  }
  return -22;
}

int64_t cell_fd_write(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame) {
  struct domain *domain = current_domain();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP) {
    int64_t wrote = cell_socket_tcp_write_from_domain(domain, file, buf, len);
    if (wrote != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return wrote; }
    return cell_block_current_on_socket_write_timeout(fd, buf, len, file->so_sndtimeo_ticks, frame);
  }
  if (file->type == OPEN_EVENTFD) {
    int64_t wrote = cell_eventfd_write_from_domain(domain, file, buf, len);
    return wrote == -EAGAIN && (file->flags & CELL_O_NONBLOCK) == 0 && frame != NULL ? -EAGAIN : wrote;
  }
  if (file->type == OPEN_PIPE) {
    int64_t wrote = cell_pipe_write_from_domain(domain, file, buf, len);
    if (wrote != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return wrote; }
    return cell_block_current_on_pipe(fd, buf, len, true, frame);
  }
  if (file->type == OPEN_UNIX_STREAM) {
    int64_t wrote = cell_pipe_write_id_from_domain(domain, file->unix_tx_pipe, buf, len);
    if (wrote != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return wrote; }
    return cell_block_current_on_pipe(fd, buf, len, true, frame);
  }
  if (file->type != OPEN_STDOUT) {
    if (file->type != OPEN_RAMFS ||
        ((file->flags & CELL_O_ACCMODE) != CELL_O_WRONLY && (file->flags & CELL_O_ACCMODE) != CELL_O_RDWR)) {
      return -22;
    }
    if (file->node.device != RAMFS_DEV_NONE) { return write_device(file, domain, buf, len); }
    if ((file->flags & CELL_O_APPEND) != 0) {
      struct vfs_node fresh;
      if (vfs_refresh(&file->node, &fresh)) { file->offset = fresh.size; }
    }
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > FILE_IO_CHUNK) { chunk = FILE_IO_CHUNK; }
      if (!vmm_copy_from_user(cell_domain_as(domain), file_io_tmp, buf + done, (size_t)chunk)) { return -14; }
      int64_t wrote = vfs_write(&file->node, file->offset, file_io_tmp, chunk);
      if (wrote < 0) { return done == 0 ? wrote : (int64_t)done; }
      if (wrote == 0) { return done == 0 ? -28 : (int64_t)done; }
      file->offset += (uint64_t)wrote;
      done += (uint64_t)wrote;
      (void)vfs_refresh(&file->node, &file->node);
    }
    return (int64_t)done;
  }
  return write_console_from_user(domain, buf, len);
}

static int64_t deliver_tty_signal_or_eintr(struct trap_frame *frame) {
  int signal = cell_tty_take_pending_signal();
  if (signal == 0) { return -EINTR; }
  if (frame == NULL || cell_current_thread_internal() == NULL) { return -EINTR; }
  cell_current_thread_internal()->tf = *frame;
  cell_current_thread_internal()->tf.x[0] = (uint64_t)(-(int64_t)EINTR);
  (void)cell_deliver_signal_to_thread(cell_current_thread_internal(), signal);
  *frame = cell_current_thread_internal()->tf;
  return CELL_SWITCHED;
}

int64_t cell_fd_read(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame) {
  struct domain *domain = current_domain();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_EVENTFD) {
    int64_t got = cell_eventfd_read_to_domain(domain, file, buf, len);
    return got == -EAGAIN && (file->flags & CELL_O_NONBLOCK) == 0 && frame != NULL ? -EAGAIN : got;
  }
  if (file->type == OPEN_PIPE) {
    int64_t got = cell_pipe_read_to_domain(domain, file, buf, len);
    if (got != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return got; }
    return cell_block_current_on_pipe(fd, buf, len, false, frame);
  }
  if (file->type == OPEN_UNIX_STREAM) {
    int64_t got = cell_pipe_read_id_to_domain(domain, file->unix_rx_pipe, buf, len);
    if (got != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return got; }
    return cell_block_current_on_pipe(fd, buf, len, false, frame);
  }
  if (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP) {
    net_poll();
    int64_t got = cell_socket_tcp_read_to_domain(domain, file, buf, len);
    if (got != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return got; }
    return cell_block_current_on_socket_timeout(fd, buf, len, 0, 0, file->so_rcvtimeo_ticks, frame);
  }
  if (file->type == OPEN_STDIN) {
    if (len == 0) { return 0; }
    int64_t n = cell_tty_read_to_user(domain, buf, len);
    if (n == -EINTR) { return deliver_tty_signal_or_eintr(frame); }
    if (n != 0) { return n; }
    if ((file->flags & CELL_O_NONBLOCK) != 0) { return -EAGAIN; }
    if (frame == NULL) { return 0; }
    cell_save_current(frame);
    cell_current_thread_internal()->state = THREAD_BLOCKED;
    cell_current_thread_internal()->running_cpu = -1;
    cell_current_thread_internal()->wait_reason = WAIT_STDIN;
    cell_current_thread_internal()->stdin_buf = buf;
    cell_current_thread_internal()->stdin_len = len;
    cell_schedule(frame);
    return CELL_SWITCHED;
  }
  if (file->type != OPEN_RAMFS || file->node.is_dir) { return -22; }
  if (file->node.device != RAMFS_DEV_NONE) { return read_device(file, domain, buf, len, frame); }
  uint64_t done = 0;
  while (done < len) {
    uint64_t chunk = len - done;
    if (chunk > FILE_IO_CHUNK) { chunk = FILE_IO_CHUNK; }
    uint64_t got = vfs_read(&file->node, file->offset, file_io_tmp, chunk);
    if (got == 0) { break; }
    if (!vmm_copy_to_user(cell_domain_as(domain), buf + done, file_io_tmp, (size_t)got)) { return -14; }
    file->offset += got;
    done += got;
  }
  return (int64_t)done;
}

int64_t cell_fd_pread(int fd, uint64_t buf, uint64_t len, uint64_t off, struct trap_frame *frame) {
  (void)frame;
  struct domain *domain = current_domain();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_RAMFS || file->node.is_dir || file->node.device != RAMFS_DEV_NONE) { return -22; }
  uint64_t done = 0;
  while (done < len) {
    uint64_t chunk = len - done;
    if (chunk > FILE_IO_CHUNK) { chunk = FILE_IO_CHUNK; }
    uint64_t got = vfs_read(&file->node, off + done, file_io_tmp, chunk);
    if (got == 0) { break; }
    if (!vmm_copy_to_user(cell_domain_as(domain), buf + done, file_io_tmp, (size_t)got)) { return -14; }
    done += got;
  }
  return (int64_t)done;
}

int64_t cell_fd_pwrite(int fd, uint64_t buf, uint64_t len, uint64_t off, struct trap_frame *frame) {
  (void)frame;
  struct domain *domain = current_domain();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_RAMFS ||
      ((file->flags & CELL_O_ACCMODE) != CELL_O_WRONLY && (file->flags & CELL_O_ACCMODE) != CELL_O_RDWR) ||
      file->node.device != RAMFS_DEV_NONE) {
    return -22;
  }
  uint64_t done = 0;
  while (done < len) {
    uint64_t chunk = len - done;
    if (chunk > FILE_IO_CHUNK) { chunk = FILE_IO_CHUNK; }
    if (!vmm_copy_from_user(cell_domain_as(domain), file_io_tmp, buf + done, (size_t)chunk)) { return -14; }
    int64_t wrote = vfs_write(&file->node, off + done, file_io_tmp, chunk);
    if (wrote < 0) { return done == 0 ? wrote : (int64_t)done; }
    if (wrote == 0) { return done == 0 ? -28 : (int64_t)done; }
    done += (uint64_t)wrote;
    (void)vfs_refresh(&file->node, &file->node);
  }
  return (int64_t)done;
}
