#include "abi/linux/shared/layout.h"
#include "proc/poll.h"
#include "poll_internal.h"
#include "fs/notify.h"
#include "net/net.h"
#include "proc/signal.h"

#include "arch/regs.h"
#include "proc/accounting.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/pipe.h"
#include "tty/pty.h"
#include "net/socket.h"
#include "proc/thread.h"
#include "tty/tty.h"

enum { EFAULT = 14, EINVAL = 22 };

static bool poll_waking;

int cell_file_poll_events(struct open_file *file, int events) {
  if (!file) { return CELL_POLLNVAL; }
  if (file->type == OPEN_URING) { return uring_poll(&file->ring, events); }
  if (file->type == OPEN_SOCKET || file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER) {
    return cell_socket_poll_events(file, events);
  }

  int revents = 0;
  if (cell_pipe_file_error(file)) { revents |= CELL_POLLERR; }
  if (file->type == OPEN_PIPE && cell_pipe_file_hup(file)) { revents |= CELL_POLLHUP; }
  if (file->type == OPEN_PTY && cell_pty_file_hup(file)) { revents |= CELL_POLLHUP; }
  if ((events & CELL_POLLIN) != 0) {
    if (file->type == OPEN_STDIN ||
        (file->type == OPEN_VFS && (file->node.device == RAMFS_DEV_TTY || file->node.device == RAMFS_DEV_CONSOLE))) {
      if (cell_tty_stdin_readable()) { revents |= CELL_POLLIN; }
    } else if (file->type == OPEN_PIPE) {
      if (cell_pipe_file_readable(file)) { revents |= CELL_POLLIN; }
    } else if (file->type == OPEN_PTY) {
      if (cell_pty_file_readable(file)) { revents |= CELL_POLLIN; }
    } else if (file->type == OPEN_VFS) {
      revents |= CELL_POLLIN;
    } else if (file->type == OPEN_EPOLL) {
      if (file->epoll_ready_head) { revents |= CELL_POLLIN; }
    } else if (file->type == OPEN_INOTIFY) {
      if (inotify_bytes(file)) { revents |= CELL_POLLIN; }
    } else if (file->type == OPEN_EVENTFD) {
      if (file->eventfd_value != 0) { revents |= CELL_POLLIN; }
    }
  }

  if ((events & CELL_POLLOUT) != 0) {
    if (file->type == OPEN_STDOUT || file->type == OPEN_STDIN || file->type == OPEN_VFS ||
        (file->type == OPEN_EVENTFD && file->eventfd_value < UINT64_MAX - 1)) {
      revents |= CELL_POLLOUT;
    } else if (file->type == OPEN_PIPE) {
      if (cell_pipe_file_writable(file)) { revents |= CELL_POLLOUT; }
    } else if (file->type == OPEN_PTY) {
      if (cell_pty_file_writable(file)) { revents |= CELL_POLLOUT; }
    }
  }
  return revents;
}

static int64_t ppoll_check(struct thread *thread, bool commit) {
  if (thread == NULL || thread->domain == NULL || thread->wait.poll_nfds > MAX_FDS) { return -EINVAL; }
  int64_t ready = 0;
  for (uint64_t i = 0; i < thread->wait.poll_nfds; ++i) {
    struct pollfd64 pfd;
    uint64_t addr = thread->wait.poll_fds + i * sizeof(pfd);
    if (!vmm_copy_from_user(cell_domain_as(thread->domain), &pfd, addr, sizeof(pfd))) { return -EFAULT; }

    int revents = 0;
    if (pfd.fd >= 0) { revents = cell_file_poll_events(cell_file_at(thread->domain, pfd.fd), pfd.events); }
    if (revents != 0) { ++ready; }
    if (commit) {
      pfd.revents = (int16_t)revents;
      if (!vmm_copy_to_user(cell_domain_as(thread->domain), addr, &pfd, sizeof(pfd))) { return -EFAULT; }
    }
  }
  return ready;
}

enum { SELECT_WORDS = (MAX_FDS + 63) / 64 };

static int64_t pselect_check(struct thread *thread, bool commit) {
  struct thread_wait *wait = &thread->wait;
  struct user_address_space *as = cell_domain_as(thread->domain);
  uint64_t sets[3][SELECT_WORDS] = {0};
  uint64_t addresses[] = {wait->poll_readfds, wait->poll_writefds, wait->poll_exceptfds};
  size_t words = (wait->poll_nfds + 63) / 64, bytes = words * sizeof(uint64_t);
  for (unsigned i = 0; i < 3; ++i) {
    if (addresses[i] && !vmm_copy_from_user(as, sets[i], addresses[i], bytes)) { return -EFAULT; }
  }
  int64_t ready = 0;
  for (size_t i = 0; i < words; ++i) {
    uint64_t read = sets[0][i], write = sets[1][i], bits = read | write | sets[2][i];
    sets[0][i] = sets[1][i] = sets[2][i] = 0;
    if (i + 1 == words && wait->poll_nfds % 64) { bits &= (UINT64_C(1) << (wait->poll_nfds % 64)) - 1; }
    while (bits) {
      unsigned pos = (unsigned)__builtin_ctzll(bits);
      uint64_t bit = UINT64_C(1) << pos;
      int events = (read & bit ? CELL_POLLIN : 0) | (write & bit ? CELL_POLLOUT : 0);
      int revents = cell_file_poll_events(cell_file_at(thread->domain, 64 * i + pos), events);
      if (revents & CELL_POLLNVAL) { return -9; }
      if ((read & bit) && (revents & (CELL_POLLIN | CELL_POLLHUP | CELL_POLLERR))) { sets[0][i] |= bit; ++ready; }
      if ((write & bit) && (revents & (CELL_POLLOUT | CELL_POLLERR))) { sets[1][i] |= bit; ++ready; }
      bits &= bits - 1;
    }
  }
  if (commit) {
    for (unsigned i = 0; i < 3; ++i) {
      if (addresses[i] && !vmm_copy_to_user(as, addresses[i], sets[i], bytes)) { return -EFAULT; }
    }
  }
  return ready;
}

static int64_t poll_wait_check(struct thread *thread, bool commit) {
  if (thread->wait.poll_kind == 1) { return ppoll_check(thread, commit); }
  if (thread->wait.poll_kind == 2) { return pselect_check(thread, commit); }
  return -EINVAL;
}

static void wake_poll_waiters(void) {
  if (poll_waking) { return; }
  poll_waking = true;
  cell_epoll_sample_terminals();
  size_t cursor = 0;
  struct thread *thread;
  while ((thread = cell_next_waiter(WAIT_POLL, &cursor))) {
    int64_t ready = poll_wait_check(thread, false);
    bool expired = thread->wait.has_deadline && cell_uptime_ticks() >= thread->wait.deadline_tick;
    if (ready != 0 || expired) {
      if (ready >= 0) { ready = poll_wait_check(thread, true); }
      cell_finish_wait(thread, ready);
    }
  }
  poll_waking = false;
  cell_wake_epoll_waiters();
}

void cell_wake_poll_waiters_internal(void) {
  wake_poll_waiters();
}

int cell_sleep_poll(struct thread *thread, struct trap_frame *frame, const uint64_t *mask,
                      bool has_timeout, uint64_t timeout_ticks) {
  if (mask != NULL) {
    thread->wait.saved_signal_mask = thread->signal_mask;
    thread->wait.restore_signal_mask = true;
    thread->signal_mask = cell_sanitize_signal_mask(*mask);
  }
  cell_mark_blocked(thread);
  if ((thread->pending_signals | thread->domain->pending_signals) && cell_deliver_pending_signals_current(frame)) {
    return CELL_SWITCHED;
  }
  if (has_timeout && timeout_ticks == 0) {
    int ready = thread->wait.poll_kind == 2 ? pselect_check(thread, true) : 0;
    cell_resume_wait(thread);
    return ready;
  }
  thread->wait.has_deadline = has_timeout;
  if (has_timeout) { thread->wait.deadline_tick = cell_uptime_ticks() + timeout_ticks; }
  thread->running_cpu = -1;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_ppoll_current(uint64_t fds, uint64_t nfds, bool has_timeout, uint64_t timeout_ticks,
                       const uint64_t *mask, struct trap_frame *frame) {
  struct thread *thread = cell_current_thread_internal();
  if (thread == NULL || cell_current_domain_internal() == NULL || nfds > MAX_FDS) { return -EINVAL; }
  net_poll();
  cell_epoll_sample_terminals();
  thread->wait.poll_kind = 1;
  thread->wait.poll_fds = fds;
  thread->wait.poll_nfds = nfds;
  int64_t ready = ppoll_check(thread, true);
  if (ready != 0) {
    cell_clear_wait(thread);
    return (int)ready;
  }
  thread->wait.reason = WAIT_POLL;
  return cell_sleep_poll(thread, frame, mask, has_timeout, timeout_ticks);
}

int cell_pselect6_current(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, bool has_timeout,
                          uint64_t timeout_ticks, const uint64_t *mask, struct trap_frame *frame) {
  struct thread *thread = cell_current_thread_internal();
  if (thread == NULL || cell_current_domain_internal() == NULL || nfds > MAX_FDS) { return -EINVAL; }
  size_t bytes = ((nfds + 63) / 64) * sizeof(uint64_t);
  uint64_t sets[] = {readfds, writefds, exceptfds};
  for (unsigned i = 0; i < 3; ++i) {
    if (sets[i] && !cell_domain_ensure_user_range(thread->domain, sets[i], bytes, VMM_ACCESS_WRITE)) { return -EFAULT; }
  }
  net_poll();
  cell_epoll_sample_terminals();
  thread->wait.poll_kind = 2;
  thread->wait.poll_nfds = nfds;
  thread->wait.poll_readfds = readfds;
  thread->wait.poll_writefds = writefds;
  thread->wait.poll_exceptfds = exceptfds;
  int64_t ready = pselect_check(thread, false);
  if (ready > 0) { ready = pselect_check(thread, true); }
  if (ready != 0) {
    cell_clear_wait(thread);
    return (int)ready;
  }
  thread->wait.reason = WAIT_POLL;
  return cell_sleep_poll(thread, frame, mask, has_timeout, timeout_ticks);
}
