#include "cell.h"

#include "exec/stack.h"
#include "kprintf.h"
#include "kstr.h"
#include "mem.h"
#include "net.h"
#include "pl011.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/futex.h"
#include "proc/memory.h"
#include "proc/pipe.h"
#include "proc/poll.h"
#include "proc/process.h"
#include "proc/procfs.h"
#include "proc/snapshot.h"
#include "proc/signal.h"
#include "proc/socket.h"
#include "proc/thread.h"
#include "proc/tty.h"
#include "random.h"
#include "vfs.h"
#include "virtio_blk.h"

#include <stddef.h>

static uint64_t scheduler_ticks;
static uint64_t scheduler_idle_ticks;
static uint64_t boot_epoch_sec;
static bool pipe_waking;

static void wake_pipe_waiters(void);

enum {
  CELL_O_ACCMODE = 3,
  CELL_O_WRONLY = 1,
  CELL_O_RDWR = 2,
  CELL_O_NONBLOCK = 04000,
  CELL_O_APPEND = 02000,
  CELL_O_CLOEXEC = 02000000,
  SOCK_STREAM = 1,
  SOCK_DGRAM = 2,
  IPPROTO_TCP = 6,
  IPPROTO_UDP = 17,
  IPPROTO_ICMP = 1,
  EPERM = 1,
  EMSGSIZE = 90,
  EAGAIN = 11,
  EFAULT = 14,
  EINVAL = 22,
  EIO = 5,
  EINPROGRESS = 115,
  ENOTCONN = 107,
  ECHILD = 10,
  EPIPE = 32,
  ENXIO = 6,
  ECONNREFUSED = 111,
  EADDRINUSE = 98,
  ESRCH = 3,
  WNOHANG = 1,
  EINTR = 4,
  CELL_S_IFMT = 0170000,
  CELL_S_IFIFO = 0010000,
};

enum tcp_socket_state {
  TCP_CLOSED,
  TCP_SYN_SENT,
  TCP_ESTABLISHED,
  TCP_FIN_WAIT,
};

static struct domain *current_domain(void) {
  return cell_current_domain_internal();
}

void cell_system_init(uint64_t hhdm_offset) {
  vma_set_hhdm_offset(hhdm_offset);
  cell_domain_reset();
  cell_thread_reset();
  cell_snapshot_reset();
  cell_fd_table_reset();
  cell_pipe_reset();
  scheduler_ticks = 0;
  scheduler_idle_ticks = 0;
  cell_tty_reset();
  // v2 Phase A object model: domains own isolation/policy state, threads own
  // EL0 execution state. The kernel remains run-to-completion on one core, so
  // these tables intentionally have no locks until a later SMP/preemptive goal.
}

bool cell_create_init(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t sp) {
  struct domain *domain = cell_alloc_domain();
  if (domain == NULL) { return false; }
  struct thread *thread = cell_alloc_thread(domain);
  if (thread == NULL) {
    domain->used = false;
    return false;
  }
  domain->parent_id = 0;
  domain->pgrp_id = domain->id;
  domain->session_id = domain->id;
  (void)cell_tty_set_foreground_pgrp(domain->pgrp_id);
  static const char *init_argv[] = {"/sbin/init"};
  cell_set_domain_identity(domain, "/sbin/init", init_argv, 1);
  domain->as = *as;
  domain->as.asid = 0;
  domain->vmas = *vmas;
  vma_list_init(vmas);
  if (!vma_insert(&domain->vmas, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP, VMM_USER_READ | VMM_USER_WRITE, 0,
                  VMA_STACK)) {
    thread->state = THREAD_UNUSED;
    vma_list_destroy(&domain->vmas);
    domain->used = false;
    return false;
  }
  if (!cell_init_stdio(domain)) {
    thread->state = THREAD_UNUSED;
    vma_list_destroy(&domain->vmas);
    domain->used = false;
    return false;
  }
  thread->state = THREAD_RUNNABLE;
  thread->tf.elr_el1 = entry;
  thread->tf.sp_el0 = sp;
  thread->tf.spsr_el1 = 0x340;
  cell_set_current_thread(thread);
  kprintf("[spore] booting... domain %d / thread %d\n", domain->id, thread->tid);
  return true;
}

void cell_set_boot_epoch(uint64_t epoch_sec) {
  boot_epoch_sec = epoch_sec;
}

uint64_t cell_realtime_seconds(void) {
  return boot_epoch_sec + scheduler_ticks / 100;
}

void cell_timer_tick(struct trap_frame *frame, bool from_lower_el) {
  ++scheduler_ticks;
  if (cell_scheduler_waiting_for_interrupt()) { ++scheduler_idle_ticks; }
  cell_wake_sleep_waiters(scheduler_ticks);
  net_poll();
  cell_wake_poll_waiters_internal();
  struct domain *domain = current_domain();
  if (domain == NULL) { return; }
  if (from_lower_el) { ++domain->cpu_ticks; }
  if (domain->budget.max_ticks != 0 && domain->budget.remaining_ticks != 0) {
    --domain->budget.remaining_ticks;
    if (domain->budget.remaining_ticks == 0) {
      kprintf("[spore] domain %d exceeded CPU budget -> killed\n", domain->id);
      if (from_lower_el) {
        cell_exit_group_current(137, frame);
      } else {
        domain->zombie = true;
        domain->exit_status = 137;
      }
      return;
    }
  }
  if (from_lower_el) { cell_schedule(frame); }
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
  case RAMFS_DEV_BLK_BOOT:
    return -22;
  case RAMFS_DEV_BLK_ROOT: {
    uint8_t tmp[128];
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
      if (!vmm_copy_from_user(&domain->as, tmp, buf + done, (size_t)chunk)) { return -14; }
      if (!virtio_blk_write(file->offset, tmp, (uint32_t)chunk)) { return -5; }
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
    uint8_t tmp[128];
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
      if (file->node.device == RAMFS_DEV_RANDOM || file->node.device == RAMFS_DEV_URANDOM) {
        random_bytes(tmp, (size_t)chunk);
      } else {
        kmemset(tmp, 0, (size_t)chunk);
      }
      if (!vmm_copy_to_user(&domain->as, buf + done, tmp, (size_t)chunk)) {
        kmemset(tmp, 0, sizeof(tmp));
        return -14;
      }
      done += chunk;
    }
    kmemset(tmp, 0, sizeof(tmp));
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
    return cell_procfs_read_device(file, domain, buf, len);
  case RAMFS_DEV_BLK_ROOT: {
    uint8_t tmp[128];
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
      if (!virtio_blk_read(file->offset, tmp, (uint32_t)chunk)) { return done == 0 ? -5 : (int64_t)done; }
      if (!vmm_copy_to_user(&domain->as, buf + done, tmp, (size_t)chunk)) { return -14; }
      file->offset += chunk;
      done += chunk;
    }
    return (int64_t)done;
  }
  case RAMFS_DEV_BLK_BOOT: {
    uint8_t tmp[128];
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
      if (!virtio_blk_read_boot(file->offset, tmp, (uint32_t)chunk)) { return done == 0 ? -5 : (int64_t)done; }
      if (!vmm_copy_to_user(&domain->as, buf + done, tmp, (size_t)chunk)) { return -14; }
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
    return cell_socket_tcp_write_from_domain(domain, file, buf, len);
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
    uint8_t tmp[128];
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
      if (!vmm_copy_from_user(&domain->as, tmp, buf + done, (size_t)chunk)) { return -14; }
      int64_t wrote = vfs_write(&file->node, file->offset, tmp, chunk);
      if (wrote < 0) { return -28; }
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
    return cell_block_current_on_socket(fd, buf, len, 0, 0, frame);
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
    cell_current_thread_internal()->wait_reason = WAIT_STDIN;
    cell_current_thread_internal()->stdin_buf = buf;
    cell_current_thread_internal()->stdin_len = len;
    cell_schedule(frame);
    return CELL_SWITCHED;
  }
  if (file->type != OPEN_RAMFS || file->node.is_dir) { return -22; }
  if (file->node.device != RAMFS_DEV_NONE) { return read_device(file, domain, buf, len, frame); }
  uint8_t tmp[128];
  uint64_t done = 0;
  while (done < len) {
    uint64_t chunk = len - done;
    if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
    uint64_t got = vfs_read(&file->node, file->offset, tmp, chunk);
    if (got == 0) { break; }
    if (!vmm_copy_to_user(&domain->as, buf + done, tmp, (size_t)got)) { return -14; }
    file->offset += got;
    done += got;
  }
  return (int64_t)done;
}

static void clear_pipe_wait(struct thread *thread) {
  thread->wait_target = -1;
  thread->pipe_buf = 0;
  thread->pipe_len = 0;
  thread->socket_addr = 0;
  thread->socket_addrlen = 0;
  thread->pipe_write = false;
}

static void wake_pipe_waiters(void) {
  if (pipe_waking) { return; }
  pipe_waking = true;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_PIPE ||
        thread->domain == NULL) {
      continue;
    }
    int fd = thread->wait_target;
    if (fd < 0 || fd >= MAX_FDS || thread->domain->fds[fd] == NULL) {
      thread->tf.x[0] = (uint64_t)-9;
    } else {
      struct open_file *file = thread->domain->fds[fd];
      int64_t rc;
      if (file->type == OPEN_UNIX_STREAM) {
        rc = thread->pipe_write
               ? cell_pipe_write_id_from_domain(thread->domain, file->unix_tx_pipe, thread->pipe_buf, thread->pipe_len)
               : cell_pipe_read_id_to_domain(thread->domain, file->unix_rx_pipe, thread->pipe_buf, thread->pipe_len);
      } else {
        rc = thread->pipe_write ? cell_pipe_write_from_domain(thread->domain, file, thread->pipe_buf, thread->pipe_len)
                                : cell_pipe_read_to_domain(thread->domain, file, thread->pipe_buf, thread->pipe_len);
      }
      if (rc == -EAGAIN) { continue; }
      thread->tf.x[0] = (uint64_t)rc;
    }
    thread->state = THREAD_RUNNABLE;
    thread->wait_reason = WAIT_NONE;
    clear_pipe_wait(thread);
  }
  pipe_waking = false;
}

void cell_pipe_notify(void) {
  wake_pipe_waiters();
  cell_wake_poll_waiters_internal();
}

int cell_sleep_current(uint64_t timeout_ticks, struct trap_frame *frame) {
  if (cell_current_thread_internal() == NULL || current_domain() == NULL) { return -EINVAL; }
  if (timeout_ticks == 0) { return 0; }
  return cell_block_current_on_sleep(scheduler_ticks + timeout_ticks, frame);
}

static struct thread *tty_signal_target_for_domain(struct domain *domain) {
  struct thread *fallback = NULL;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state == THREAD_UNUSED || thread->domain != domain) { continue; }
    if (fallback == NULL) { fallback = thread; }
    if (thread->state == THREAD_BLOCKED && thread->wait_reason == WAIT_STDIN) { return thread; }
  }
  return fallback;
}

static void tty_deliver_pending_signal_to_foreground(struct trap_frame *frame) {
  int signal = cell_tty_take_pending_signal();
  if (signal == 0) { return; }
  int pgrp = cell_tty_foreground_pgrp();
  if (pgrp <= 0) { return; }
  struct thread *interrupted = cell_current_thread_internal();
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    struct domain *domain = cell_domain_slot(i);
    if (domain == NULL || !domain->used || domain->zombie || domain->pgrp_id != pgrp) { continue; }
    struct thread *thread = tty_signal_target_for_domain(domain);
    if (thread == interrupted && frame != NULL) {
      cell_signal_current(signal, frame);
    } else {
      (void)cell_deliver_signal_to_thread(thread, signal);
    }
  }
}

void cell_wake_stdin(struct trap_frame *frame) {
  cell_tty_process_input();
  tty_deliver_pending_signal_to_foreground(frame);
  cell_wake_poll_waiters_internal();
  cell_wake_epoll_waiters_internal();
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_STDIN) { continue; }
    int64_t n = cell_tty_read_to_user(thread->domain, thread->stdin_buf, thread->stdin_len);
    if (n == -EINTR && cell_tty_pending_signal() != 0) {
      int signal = cell_tty_take_pending_signal();
      (void)cell_deliver_signal_to_thread(thread, signal);
      continue;
    }
    if (n <= 0) { continue; }
    thread->tf.x[0] = (uint64_t)n;
    thread->state = THREAD_RUNNABLE;
    thread->wait_reason = WAIT_NONE;
    thread->stdin_buf = 0;
    thread->stdin_len = 0;
  }
  cell_wake_poll_waiters_internal();
}

static void wake_socket_waiters(struct open_file *file) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_SOCKET ||
        thread->domain == NULL) {
      continue;
    }
    int fd = thread->wait_target;
    if (fd >= 0 && fd < MAX_FDS && thread->domain->fds[fd] == file) {
      if (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP) {
        if (thread->pipe_buf == 0 && thread->pipe_len == 0) {
          if (file->tcp_error != 0) {
            thread->tf.x[0] = (uint64_t)(-(int64_t)file->tcp_error);
          } else if (file->tcp_state != TCP_ESTABLISHED) {
            continue;
          } else {
            thread->tf.x[0] = 0;
          }
        } else {
          int64_t rc = cell_socket_tcp_read_to_domain(thread->domain, file, thread->pipe_buf, thread->pipe_len);
          if (rc == -EAGAIN) { continue; }
          thread->tf.x[0] = (uint64_t)rc;
        }
      } else if (file->type == OPEN_SOCKET && file->udp_rx_len != 0 && thread->pipe_buf != 0) {
        uint64_t n = file->udp_rx_len < thread->pipe_len ? file->udp_rx_len : thread->pipe_len;
        if (!vmm_copy_to_user(&thread->domain->as, thread->pipe_buf, file->udp_rx, (size_t)n)) {
          thread->tf.x[0] = (uint64_t)(-(int64_t)EFAULT);
        } else if (!cell_socket_copy_udp_source_to_domain(thread->domain, file, thread->socket_addr,
                                                          thread->socket_addrlen)) {
          thread->tf.x[0] = (uint64_t)(-(int64_t)EFAULT);
        } else {
          file->udp_rx_len = 0;
          thread->tf.x[0] = (uint64_t)n;
        }
      }
      thread->state = THREAD_RUNNABLE;
      thread->wait_reason = WAIT_NONE;
      thread->wait_target = -1;
      thread->pipe_buf = 0;
      thread->pipe_len = 0;
      thread->socket_addr = 0;
      thread->socket_addrlen = 0;
    }
  }
  cell_wake_poll_waiters_internal();
}

void cell_socket_wake_file(struct open_file *file) {
  wake_socket_waiters(file);
}

uint64_t cell_uptime_ticks(void) {
  return scheduler_ticks;
}

uint64_t cell_idle_ticks(void) {
  return scheduler_idle_ticks;
}

uint64_t cell_boot_epoch_seconds(void) {
  return boot_epoch_sec;
}

bool cell_proc_exists(int pid) {
  return cell_find_domain(pid) != NULL;
}

uint32_t cell_proc_uid(int pid) {
  struct domain *domain = cell_find_domain(pid);
  return domain == NULL ? 0 : domain->uid;
}

uint32_t cell_proc_gid(int pid) {
  struct domain *domain = cell_find_domain(pid);
  return domain == NULL ? 0 : domain->gid;
}

void cell_note_unsupported_syscall(uint64_t nr) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return; }
  ++domain->unsupported_syscalls;
  domain->last_unsupported_syscall = nr;
}

int cell_proc_pid_at(size_t index) {
  size_t seen = 0;
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    struct domain *domain = cell_domain_slot(i);
    if (domain == NULL || !domain->used) { continue; }
    if (seen == index) { return domain->id; }
    ++seen;
  }
  return 0;
}
