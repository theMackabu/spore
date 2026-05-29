#include "proc/poll.h"

#include "net.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/pipe.h"
#include "proc/socket.h"
#include "proc/thread.h"
#include "proc/tty.h"

enum {
  CELL_O_CLOEXEC = 02000000,
  IPPROTO_TCP = 6,
  EAGAIN = 11,
  EFAULT = 14,
  EINVAL = 22,
  EINTR = 4,
  EPOLL_CTL_ADD = 1,
  EPOLL_CTL_DEL = 2,
  EPOLL_CTL_MOD = 3,
};

enum tcp_socket_state {
  TCP_CLOSED,
  TCP_SYN_SENT,
  TCP_ESTABLISHED,
  TCP_FIN_WAIT,
};

struct epoll_event64 {
  uint32_t events;
  uint64_t data;
};

struct pollfd64 {
  int32_t fd;
  int16_t events;
  int16_t revents;
};

static bool poll_waking;
static bool epoll_waking;

static int fd_poll_events_for_domain(struct domain *domain, int fd, int events) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return CELL_POLLNVAL; }

  struct open_file *file = domain->fds[fd];
  int revents = 0;
  if (file->type == OPEN_PIPE && cell_pipe_file_hup(file)) { revents |= CELL_POLLHUP; }
  if (file->type == OPEN_UNIX_STREAM && cell_pipe_id_hup(file->unix_rx_pipe)) { revents |= CELL_POLLHUP; }
  if ((events & CELL_POLLIN) != 0) {
    if (file->type == OPEN_STDIN ||
        (file->type == OPEN_RAMFS && (file->node.device == RAMFS_DEV_TTY || file->node.device == RAMFS_DEV_CONSOLE))) {
      if (cell_tty_stdin_readable()) { revents |= CELL_POLLIN; }
    } else if (file->type == OPEN_SOCKET) {
      net_poll();
      if (file->socket_proto == IPPROTO_TCP) {
        if (file->tcp_rx_len != 0 || file->tcp_fin || file->tcp_error != 0) { revents |= CELL_POLLIN; }
      } else if (file->udp_rx_len != 0) {
        revents |= CELL_POLLIN;
      }
    } else if (file->type == OPEN_PIPE) {
      if (cell_pipe_file_readable(file)) { revents |= CELL_POLLIN; }
    } else if (file->type == OPEN_UNIX_LISTENER) {
      if (cell_unix_listener_readable(file)) { revents |= CELL_POLLIN; }
    } else if (file->type == OPEN_UNIX_STREAM) {
      if (cell_pipe_id_readable(file->unix_rx_pipe)) { revents |= CELL_POLLIN; }
    } else if (file->type == OPEN_RAMFS) {
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
    if (file->type == OPEN_STDOUT || file->type == OPEN_STDIN || file->type == OPEN_RAMFS || file->type == OPEN_EPOLL ||
        file->type == OPEN_EVENTFD) {
      revents |= CELL_POLLOUT;
    } else if (file->type == OPEN_SOCKET) {
      if (file->socket_proto != IPPROTO_TCP || file->tcp_state == TCP_ESTABLISHED || file->tcp_error != 0) {
        revents |= CELL_POLLOUT;
      }
    } else if (file->type == OPEN_PIPE) {
      if (cell_pipe_file_writable(file)) { revents |= CELL_POLLOUT; }
    } else if (file->type == OPEN_UNIX_STREAM) {
      if (cell_pipe_id_writable(file->unix_tx_pipe)) { revents |= CELL_POLLOUT; }
    }
  }
  return revents;
}

int cell_fd_poll_events(int fd, int events) {
  return fd_poll_events_for_domain(cell_current_domain_internal(), fd, events);
}

static int64_t ppoll_check(struct thread *thread, bool commit) {
  if (thread == NULL || thread->domain == NULL || thread->poll_nfds > CELL_MAX_POLL_FDS) { return -EINVAL; }
  int64_t ready = 0;
  for (uint64_t i = 0; i < thread->poll_nfds; ++i) {
    struct pollfd64 pfd;
    uint64_t addr = thread->poll_fds + i * sizeof(pfd);
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
  if (thread == NULL || thread->domain == NULL || thread->poll_nfds > 64) { return -EINVAL; }
  uint64_t read_in = 0;
  uint64_t write_in = 0;
  uint64_t read_out = 0;
  uint64_t write_out = 0;
  uint64_t except_out = 0;
  if (thread->poll_readfds != 0 &&
      !vmm_copy_from_user(cell_domain_as(thread->domain), &read_in, thread->poll_readfds, sizeof(read_in))) {
    return -EFAULT;
  }
  if (thread->poll_writefds != 0 &&
      !vmm_copy_from_user(cell_domain_as(thread->domain), &write_in, thread->poll_writefds, sizeof(write_in))) {
    return -EFAULT;
  }

  int64_t ready = 0;
  for (uint64_t fd = 0; fd < thread->poll_nfds; ++fd) {
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
    if ((thread->poll_readfds != 0 &&
         !vmm_copy_to_user(cell_domain_as(thread->domain), thread->poll_readfds, &read_out, sizeof(read_out))) ||
        (thread->poll_writefds != 0 &&
         !vmm_copy_to_user(cell_domain_as(thread->domain), thread->poll_writefds, &write_out, sizeof(write_out))) ||
        (thread->poll_exceptfds != 0 &&
         !vmm_copy_to_user(cell_domain_as(thread->domain), thread->poll_exceptfds, &except_out, sizeof(except_out)))) {
      return -EFAULT;
    }
  }
  return ready;
}

static void clear_poll_wait(struct thread *thread) {
  thread->poll_kind = 0;
  thread->poll_has_deadline = false;
  thread->poll_deadline_tick = 0;
  thread->poll_fds = 0;
  thread->poll_nfds = 0;
  thread->poll_readfds = 0;
  thread->poll_writefds = 0;
  thread->poll_exceptfds = 0;
}

static int64_t poll_wait_check(struct thread *thread, bool commit) {
  if (thread->poll_kind == 1) { return ppoll_check(thread, commit); }
  if (thread->poll_kind == 2) { return pselect_check(thread, commit); }
  return -EINVAL;
}

static void wake_epoll_waiters(void);

static void wake_poll_waiters(void) {
  if (poll_waking) { return; }
  poll_waking = true;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_POLL ||
        thread->domain == NULL) {
      continue;
    }
    int64_t ready = poll_wait_check(thread, false);
    bool expired = thread->poll_has_deadline && cell_uptime_ticks() >= thread->poll_deadline_tick;
    if (ready < 0 || ready != 0 || expired) {
      if (ready >= 0) { ready = poll_wait_check(thread, true); }
      if (ready < 0) { ready = -EFAULT; }
      thread->tf.x[0] = (uint64_t)ready;
      thread->state = THREAD_RUNNABLE;
      thread->wait_reason = WAIT_NONE;
      clear_poll_wait(thread);
    }
  }
  poll_waking = false;
  wake_epoll_waiters();
}

void cell_wake_poll_waiters_internal(void) {
  wake_poll_waiters();
}

void cell_socket_wake_unix_accept_waiters(struct open_file *listener) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_SOCKET ||
        thread->domain == NULL) {
      continue;
    }
    int fd = thread->wait_target;
    if (fd < 0 || fd >= MAX_FDS || thread->domain->fds[fd] != listener) { continue; }
    int accepted = cell_socket_take_pending_unix(thread->domain, listener);
    if (accepted == -EAGAIN) { continue; }
    thread->tf.x[0] = (uint64_t)accepted;
    thread->state = THREAD_RUNNABLE;
    thread->wait_reason = WAIT_NONE;
    thread->wait_target = -1;
  }
  wake_poll_waiters();
}

static void clear_epoll_wait(struct thread *thread) {
  thread->epoll_fd = -1;
  thread->epoll_events = 0;
  thread->epoll_maxevents = 0;
  thread->poll_has_deadline = false;
  thread->poll_deadline_tick = 0;
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
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_EPOLL ||
        thread->domain == NULL) {
      continue;
    }
    int ready =
      epoll_wait_for_domain(thread->domain, thread->epoll_fd, thread->epoll_events, thread->epoll_maxevents, false);
    bool expired = thread->poll_has_deadline && cell_uptime_ticks() >= thread->poll_deadline_tick;
    if (ready == 0 && !expired) { continue; }
    if (ready > 0) {
      ready =
        epoll_wait_for_domain(thread->domain, thread->epoll_fd, thread->epoll_events, thread->epoll_maxevents, true);
    }
    /*
     * The original syscall arguments were already validated before blocking.
     * If the saved wait can no longer be committed after an asynchronous wake
     * (for example because another thread changed the fd table), report an
     * interrupted wait instead of surfacing a fresh EINVAL/EFAULT to userland.
     */
    if (ready < 0) { ready = -EINTR; }
    thread->tf.x[0] = (uint64_t)ready;
    thread->state = THREAD_RUNNABLE;
    thread->wait_reason = WAIT_NONE;
    clear_epoll_wait(thread);
  }
  epoll_waking = false;
}

void cell_wake_epoll_waiters_internal(void) {
  wake_epoll_waiters();
}

int cell_ppoll_current(uint64_t fds, uint64_t nfds, bool has_timeout, uint64_t timeout_ticks,
                       struct trap_frame *frame) {
  struct thread *thread = cell_current_thread_internal();
  if (thread == NULL || cell_current_domain_internal() == NULL || nfds > CELL_MAX_POLL_FDS) { return -EINVAL; }
  cell_save_current(frame);
  thread->poll_kind = 1;
  thread->poll_fds = fds;
  thread->poll_nfds = nfds;
  int64_t ready = ppoll_check(thread, true);
  if (ready != 0 || (has_timeout && timeout_ticks == 0) || ready < 0) {
    clear_poll_wait(thread);
    return (int)ready;
  }
  thread->state = THREAD_BLOCKED;
  thread->running_cpu = -1;
  thread->wait_reason = WAIT_POLL;
  thread->poll_has_deadline = has_timeout;
  thread->poll_deadline_tick = cell_uptime_ticks() + timeout_ticks;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_pselect6_current(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, bool has_timeout,
                          uint64_t timeout_ticks, struct trap_frame *frame) {
  struct thread *thread = cell_current_thread_internal();
  if (thread == NULL || cell_current_domain_internal() == NULL || nfds > 64) { return -EINVAL; }
  cell_save_current(frame);
  thread->poll_kind = 2;
  thread->poll_nfds = nfds;
  thread->poll_readfds = readfds;
  thread->poll_writefds = writefds;
  thread->poll_exceptfds = exceptfds;
  int64_t ready = pselect_check(thread, false);
  if (ready > 0) { ready = pselect_check(thread, true); }
  if (ready != 0 || (has_timeout && timeout_ticks == 0) || ready < 0) {
    if (ready == 0) { ready = pselect_check(thread, true); }
    clear_poll_wait(thread);
    return (int)ready;
  }
  thread->state = THREAD_BLOCKED;
  thread->running_cpu = -1;
  thread->wait_reason = WAIT_POLL;
  thread->poll_has_deadline = has_timeout;
  thread->poll_deadline_tick = cell_uptime_ticks() + timeout_ticks;
  cell_schedule(frame);
  return CELL_SWITCHED;
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

int cell_fd_epoll_wait(int epfd, uint64_t events_addr, int maxevents) {
  struct domain *domain = cell_current_domain_internal();
  if (maxevents <= 0) { return -EINVAL; }
  if (!cell_ensure_user_range(events_addr, (size_t)maxevents * sizeof(struct epoll_event64), VMM_ACCESS_WRITE)) {
    return -EFAULT;
  }
  return epoll_wait_for_domain(domain, epfd, events_addr, maxevents, true);
}

int cell_epoll_wait_current(int epfd, uint64_t events_addr, int maxevents, int timeout_ms, struct trap_frame *frame) {
  if (maxevents <= 0) { return -EINVAL; }
  if (!cell_ensure_user_range(events_addr, (size_t)maxevents * sizeof(struct epoll_event64), VMM_ACCESS_WRITE)) {
    return -EFAULT;
  }
  struct domain *domain = cell_current_domain_internal();
  int ready = epoll_wait_for_domain(domain, epfd, events_addr, maxevents, true);
  if (ready != 0 || timeout_ms == 0 || frame == NULL) { return ready; }

  struct thread *thread = cell_current_thread_internal();
  cell_save_current(frame);
  thread->state = THREAD_BLOCKED;
  thread->running_cpu = -1;
  thread->wait_reason = WAIT_EPOLL;
  thread->epoll_fd = epfd;
  thread->epoll_events = events_addr;
  thread->epoll_maxevents = maxevents;
  thread->poll_has_deadline = timeout_ms > 0;
  thread->poll_deadline_tick = timeout_ms > 0 ? cell_uptime_ticks() + ((uint64_t)timeout_ms + 9) / 10 : 0;
  cell_schedule(frame);
  return CELL_SWITCHED;
}
