#include "abi/linux/aarch64/layout.h"
#include "proc/poll.h"
#include "proc/signal.h"

#include "arch/aarch64/regs.h"
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

enum {
  CELL_O_CLOEXEC = 02000000,
  EFAULT = 14,
  EINVAL = 22,
  EINTR = 4,
  EPOLL_CTL_ADD = 1,
  EPOLL_CTL_DEL = 2,
  EPOLL_CTL_MOD = 3,
};

static bool poll_waking;
static bool epoll_waking;

static int fd_poll_events_for_domain(struct domain *domain, int fd, int events) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return CELL_POLLNVAL; }

  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_SOCKET || file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER) {
    return cell_socket_poll_events(file, events);
  }

  int revents = 0;
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
      for (size_t i = 0; i < CELL_EPOLL_WATCH_CAP; ++i) {
        if (!file->epoll_watches[i].used) { continue; }
        int ready = fd_poll_events_for_domain(domain, file->epoll_watches[i].fd, (int)file->epoll_watches[i].events);
        if ((ready & (CELL_POLLIN | CELL_POLLOUT | CELL_POLLERR | CELL_POLLHUP | CELL_POLLNVAL)) != 0) {
          revents |= CELL_POLLIN;
          break;
        }
      }
    } else if (file->type == OPEN_EVENTFD) {
      if (file->eventfd_value != 0) { revents |= CELL_POLLIN; }
    }
  }

  if ((events & CELL_POLLOUT) != 0) {
    if (file->type == OPEN_STDOUT || file->type == OPEN_STDIN || file->type == OPEN_VFS || file->type == OPEN_EPOLL ||
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
  if (thread == NULL || thread->domain == NULL || thread->wait.poll_nfds > CELL_MAX_POLL_FDS) { return -EINVAL; }
  int64_t ready = 0;
  for (uint64_t i = 0; i < thread->wait.poll_nfds; ++i) {
    struct pollfd64 pfd;
    uint64_t addr = thread->wait.poll_fds + i * sizeof(pfd);
    if (!vmm_copy_from_user(cell_domain_as(thread->domain), &pfd, addr, sizeof(pfd))) { return -EFAULT; }

    int revents = 0;
    if (pfd.fd >= 0) { revents = fd_poll_events_for_domain(thread->domain, pfd.fd, pfd.events); }
    if (revents != 0) { ++ready; }
    if (commit) {
      pfd.revents = (int16_t)revents;
      if (!vmm_copy_to_user(cell_domain_as(thread->domain), addr, &pfd, sizeof(pfd))) { return -EFAULT; }
    }
  }
  return ready;
}

static int64_t pselect_check(struct thread *thread, bool commit) {
  if (thread == NULL || thread->domain == NULL || thread->wait.poll_nfds > 64) { return -EINVAL; }
  uint64_t read_in = 0;
  uint64_t write_in = 0;
  uint64_t read_out = 0;
  uint64_t write_out = 0;
  uint64_t except_out = 0;
  if (thread->wait.poll_readfds != 0 &&
      !vmm_copy_from_user(cell_domain_as(thread->domain), &read_in, thread->wait.poll_readfds, sizeof(read_in))) {
    return -EFAULT;
  }
  if (thread->wait.poll_writefds != 0 &&
      !vmm_copy_from_user(cell_domain_as(thread->domain), &write_in, thread->wait.poll_writefds, sizeof(write_in))) {
    return -EFAULT;
  }

  int64_t ready = 0;
  for (uint64_t fd = 0; fd < thread->wait.poll_nfds; ++fd) {
    uint64_t bit = 1ull << fd;
    int events = 0;
    if ((read_in & bit) != 0) { events |= CELL_POLLIN; }
    if ((write_in & bit) != 0) { events |= CELL_POLLOUT; }
    if (events == 0) { continue; }
    int revents = fd_poll_events_for_domain(thread->domain, (int)fd, events);
    if ((revents & CELL_POLLIN) != 0 && (read_in & bit) != 0) { read_out |= bit; }
    if ((revents & CELL_POLLOUT) != 0 && (write_in & bit) != 0) { write_out |= bit; }
    if ((revents & (CELL_POLLIN | CELL_POLLOUT | CELL_POLLERR | CELL_POLLHUP | CELL_POLLNVAL)) != 0) { ++ready; }
  }

  if (commit) {
    if ((thread->wait.poll_readfds != 0 &&
         !vmm_copy_to_user(cell_domain_as(thread->domain), thread->wait.poll_readfds, &read_out, sizeof(read_out))) ||
        (thread->wait.poll_writefds != 0 &&
         !vmm_copy_to_user(cell_domain_as(thread->domain), thread->wait.poll_writefds, &write_out, sizeof(write_out))) ||
        (thread->wait.poll_exceptfds != 0 &&
         !vmm_copy_to_user(cell_domain_as(thread->domain), thread->wait.poll_exceptfds, &except_out, sizeof(except_out)))) {
      return -EFAULT;
    }
  }
  return ready;
}

static int64_t poll_wait_check(struct thread *thread, bool commit) {
  if (thread->wait.poll_kind == 1) { return ppoll_check(thread, commit); }
  if (thread->wait.poll_kind == 2) { return pselect_check(thread, commit); }
  return -EINVAL;
}

static void wake_epoll_waiters(void);

static void wake_poll_waiters(void) {
  if (poll_waking) { return; }
  poll_waking = true;
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait.reason != WAIT_POLL ||
        thread->domain == NULL) {
      continue;
    }
    int64_t ready = poll_wait_check(thread, false);
    bool expired = thread->wait.has_deadline && cell_uptime_ticks() >= thread->wait.deadline_tick;
    if (ready != 0 || expired) {
      if (ready >= 0) { ready = poll_wait_check(thread, true); }
      if (ready < 0) { ready = -EFAULT; }
      cell_finish_wait(thread, ready);
    }
  }
  poll_waking = false;
  wake_epoll_waiters();
}

void cell_wake_poll_waiters_internal(void) {
  wake_poll_waiters();
}

static int epoll_wait_for_domain(struct domain *domain, int epfd, uint64_t events_addr, int maxevents, bool commit) {
  if (maxevents <= 0) { return -EINVAL; }
  if (domain == NULL || epfd < 0 || epfd >= MAX_FDS || domain->fds[epfd] == NULL ||
      domain->fds[epfd]->type != OPEN_EPOLL) {
    return -9;
  }
  struct open_file *ep = domain->fds[epfd];
  int written = 0;
  for (size_t i = 0; i < CELL_EPOLL_WATCH_CAP && written < maxevents; ++i) {
    if (!ep->epoll_watches[i].used) { continue; }
    int ready = fd_poll_events_for_domain(domain, ep->epoll_watches[i].fd, (int)ep->epoll_watches[i].events);
    if ((ready & (CELL_POLLIN | CELL_POLLOUT | CELL_POLLERR | CELL_POLLHUP | CELL_POLLNVAL)) == 0) { continue; }
    if (commit) {
      struct epoll_event64 out = {.events = (uint32_t)ready, .data = ep->epoll_watches[i].data};
      uint64_t dst = events_addr + (uint64_t)written * sizeof(out);
      if (!vmm_copy_to_user(cell_domain_as(domain), dst, &out, sizeof(out))) { return -EFAULT; }
    }
    ++written;
  }
  return written;
}

static void wake_epoll_waiters(void) {
  if (epoll_waking) { return; }
  epoll_waking = true;
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait.reason != WAIT_EPOLL ||
        thread->domain == NULL) {
      continue;
    }
    int ready =
      epoll_wait_for_domain(thread->domain, thread->wait.target, thread->wait.epoll_events, thread->wait.epoll_maxevents, false);
    bool expired = thread->wait.has_deadline && cell_uptime_ticks() >= thread->wait.deadline_tick;
    if (ready == 0 && !expired) { continue; }
    if (ready > 0) {
      ready =
        epoll_wait_for_domain(thread->domain, thread->wait.target, thread->wait.epoll_events, thread->wait.epoll_maxevents, true);
    }
    if (ready < 0) { ready = -EINTR; }
    cell_finish_wait(thread, ready);
  }
  epoll_waking = false;
}

void cell_wake_epoll_waiters_internal(void) {
  wake_epoll_waiters();
}

static int sleep_poll(struct thread *thread, struct trap_frame *frame, const uint64_t *mask,
                      bool has_timeout, uint64_t timeout_ticks) {
  if (mask != NULL) {
    thread->wait.saved_signal_mask = thread->signal_mask;
    thread->wait.restore_signal_mask = true;
    thread->signal_mask = cell_sanitize_signal_mask(*mask);
  }
  thread->state = THREAD_BLOCKED;
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
  if (thread == NULL || cell_current_domain_internal() == NULL || nfds > CELL_MAX_POLL_FDS) { return -EINVAL; }
  thread->wait.poll_kind = 1;
  thread->wait.poll_fds = fds;
  thread->wait.poll_nfds = nfds;
  int64_t ready = ppoll_check(thread, true);
  if (ready != 0) {
    cell_clear_wait(thread);
    return (int)ready;
  }
  thread->wait.reason = WAIT_POLL;
  return sleep_poll(thread, frame, mask, has_timeout, timeout_ticks);
}

int cell_pselect6_current(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, bool has_timeout,
                          uint64_t timeout_ticks, const uint64_t *mask, struct trap_frame *frame) {
  struct thread *thread = cell_current_thread_internal();
  if (thread == NULL || cell_current_domain_internal() == NULL || nfds > 64) { return -EINVAL; }
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
  return sleep_poll(thread, frame, mask, has_timeout, timeout_ticks);
}

int cell_fd_epoll_create(int flags) {
  if ((flags & ~CELL_O_CLOEXEC) != 0) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -24; }
  struct open_file *file = cell_alloc_open_file();
  if (file == NULL) { return -12; }
  file->type = OPEN_EPOLL;
  file->flags = (uint32_t)(flags & ~CELL_O_CLOEXEC);
  domain->fds[fd] = file;
  domain->fd_flags[fd] = (flags & CELL_O_CLOEXEC) != 0 ? 1 : 0;
  return fd;
}

int cell_fd_epoll_ctl(int epfd, int op, int fd, uint32_t events, uint64_t data) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || epfd < 0 || epfd >= MAX_FDS || domain->fds[epfd] == NULL ||
      domain->fds[epfd]->type != OPEN_EPOLL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) {
    return -9;
  }
  struct open_file *ep = domain->fds[epfd];
  int free_slot = -1;
  for (size_t i = 0; i < CELL_EPOLL_WATCH_CAP; ++i) {
    if (ep->epoll_watches[i].used && ep->epoll_watches[i].fd == fd) {
      if (op == EPOLL_CTL_DEL) {
        ep->epoll_watches[i] = (struct epoll_watch){0};
        return 0;
      }
      if (op == EPOLL_CTL_MOD || op == EPOLL_CTL_ADD) {
        ep->epoll_watches[i].events = events;
        ep->epoll_watches[i].data = data;
        return 0;
      }
      return -EINVAL;
    }
    if (!ep->epoll_watches[i].used && free_slot < 0) { free_slot = (int)i; }
  }
  if (op == EPOLL_CTL_DEL) { return -9; }
  if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD) { return -EINVAL; }
  if (free_slot < 0) { return -12; }
  ep->epoll_watches[free_slot] = (struct epoll_watch){.used = true, .fd = fd, .events = events, .data = data};
  return 0;
}

int cell_epoll_wait_current(int epfd, uint64_t events_addr, int maxevents, int timeout_ms, const uint64_t *mask, struct trap_frame *frame) {
  if (maxevents <= 0) { return -EINVAL; }
  if (!cell_ensure_user_range(events_addr, (size_t)maxevents * sizeof(struct epoll_event64), VMM_ACCESS_WRITE)) {
    return -EFAULT;
  }
  struct domain *domain = cell_current_domain_internal();
  int ready = epoll_wait_for_domain(domain, epfd, events_addr, maxevents, true);
  if (ready != 0 || timeout_ms == 0 || frame == NULL) { return ready; }

  struct thread *thread = cell_current_thread_internal();
  thread->wait = (struct thread_wait){.reason = WAIT_EPOLL, .target = epfd, .epoll_events = events_addr,
                                      .epoll_maxevents = maxevents};
  uint64_t ticks = timeout_ms > 0 ? ((uint64_t)timeout_ms + 9) / 10 : 0;
  return sleep_poll(thread, frame, mask, timeout_ms > 0, ticks);
}
