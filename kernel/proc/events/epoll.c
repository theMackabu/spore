#include "abi/linux/shared/file_layout.h"
#include "mm/pool.h"
#include "net/net.h"
#include "proc/domain.h"
#include "proc/epoll.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "proc/pipe.h"
#include "proc/poll.h"
#include "proc/accounting.h"
#include "poll_internal.h"

enum { ADD = 1, DEL = 2, MOD = 3, MAX_DEPTH = 5, ONESHOT = 1U << 30, EDGE = 1U << 31, CLOEXEC = 02000000 };

struct epoll_subscription {
  struct epoll_subscription *next, **link;
  struct epoll_watch *watch;
  uint32_t events;
};

struct epoll_watch {
  struct open_file *ep, *target;
  struct epoll_watch *next, **link, *observer_next, **observer_link;
  struct epoll_watch *ready_prev, *ready_next;
  struct epoll_subscription sources[2];
  int fd;
  uint32_t mask, last, pending;
  uint64_t data;
  bool queued, disabled;
};

static struct object_pool watches;
static struct epoll_source console_input, sampled_ptys;
static unsigned network_watches;
static uint64_t scan_generation;

static void enqueue(struct epoll_watch *w) {
  struct open_file *ep = w->ep;
  w->ready_prev = ep->epoll_ready_tail;
  if (w->ready_prev) { w->ready_prev->ready_next = w; }
  else { ep->epoll_ready_head = w; }
  ep->epoll_ready_tail = w;
  ++ep->epoll_ready_count;
  w->queued = true;
}

static void dequeue(struct epoll_watch *w) {
  struct open_file *ep = w->ep;
  if (w->ready_prev) { w->ready_prev->ready_next = w->ready_next; }
  else { ep->epoll_ready_head = w->ready_next; }
  if (w->ready_next) { w->ready_next->ready_prev = w->ready_prev; }
  else { ep->epoll_ready_tail = w->ready_prev; }
  --ep->epoll_ready_count;
  w->ready_next = w->ready_prev = NULL;
  w->queued = false;
}

static void refresh(struct epoll_watch *w, uint32_t activity) {
  if (w->disabled) { return; }
  uint32_t ready = (uint32_t)cell_file_poll_events(w->target, (int)w->mask);
  uint32_t arrivals = ready & (~w->last | activity);
  w->pending = w->mask & EDGE ? (w->pending & ready) | arrivals : ready;
  w->last = ready;
  bool was_ready = w->ep->epoll_ready_head != NULL;
  if (w->pending && !w->queued) { enqueue(w); }
  else if (!w->pending && w->queued) { dequeue(w); }
  if (w->ep->epoll_observers) {
    if (arrivals) { cell_epoll_file_activity(w->ep, CELL_POLLIN); }
    else if (was_ready != (w->ep->epoll_ready_head != NULL)) { cell_epoll_file_changed(w->ep); }
  }
}

void cell_epoll_file_activity(struct open_file *file, unsigned events) {
  for (struct epoll_watch *w = file->epoll_observers; w; w = w->observer_next) { refresh(w, events); }
}

void cell_epoll_file_changed(struct open_file *file) { cell_epoll_file_activity(file, 0); }

void cell_epoll_source_activity(struct epoll_source *source, unsigned events) {
  for (struct epoll_subscription *sub = source->first; sub; sub = sub->next) {
    refresh(sub->watch, events & sub->events);
  }
}

void cell_epoll_source_changed(struct epoll_source *source) { cell_epoll_source_activity(source, 0); }

void cell_epoll_console_activity(unsigned events) { cell_epoll_source_activity(&console_input, events); }

void cell_epoll_sample_terminals(void) {
  cell_epoll_console_activity(0);
  cell_epoll_source_changed(&sampled_ptys);
}

static void subscribe(struct epoll_watch *w, unsigned i, struct epoll_source *source, unsigned events) {
  struct epoll_subscription *sub = &w->sources[i];
  sub->watch = w;
  sub->events = events;
  sub->next = source->first;
  sub->link = &source->first;
  if (sub->next) { sub->next->link = &sub->next; }
  source->first = sub;
}

static void unsubscribe(struct epoll_watch *w) {
  for (unsigned i = 0; i < 2; ++i) {
    struct epoll_subscription *sub = &w->sources[i];
    if (sub->link) {
      *sub->link = sub->next;
      if (sub->next) { sub->next->link = sub->link; }
      sub->link = NULL;
    }
  }
}

static void attach_sources(struct epoll_watch *w) {
  struct open_file *target = w->target;
  if (target->type == OPEN_PIPE) {
    subscribe(w, 0, cell_pipe_poll_source(target->pipe), CELL_POLLIN | CELL_POLLOUT);
  } else if (target->type == OPEN_UNIX_STREAM && target->unix_connected) {
    subscribe(w, 0, cell_pipe_poll_source(target->unix_rx_pipe), CELL_POLLIN);
    subscribe(w, 1, cell_pipe_poll_source(target->unix_tx_pipe), CELL_POLLOUT);
  } else if (target->type == OPEN_PTY) {
    subscribe(w, 0, &sampled_ptys, 0);
  } else if (target->type == OPEN_STDIN || target->type == OPEN_VFS) {
    subscribe(w, 0, &console_input, CELL_POLLIN);
  }
}

void cell_epoll_sources_changed(struct open_file *file) {
  for (struct epoll_watch *w = file->epoll_observers; w; w = w->observer_next) {
    unsubscribe(w);
    attach_sources(w);
    refresh(w, 0);
  }
}

static void remove_watch(struct epoll_watch *w) {
  struct open_file *ep = w->ep;
  bool notify = w->queued && ep->epoll_ready_count == 1;
  if (w->queued) { dequeue(w); }
  if (w->target->type == OPEN_SOCKET) { --network_watches; }
  unsubscribe(w);
  *w->link = w->next;
  if (w->next) { w->next->link = w->link; }
  *w->observer_link = w->observer_next;
  if (w->observer_next) { w->observer_next->observer_link = w->observer_link; }
  pool_free(&watches, w);
  if (notify) { cell_epoll_file_changed(ep); }
}

void cell_epoll_release_file(struct open_file *file) {
  while (file->epoll_observers) { remove_watch(file->epoll_observers); }
  if (file->type == OPEN_EPOLL) {
    while (file->epoll_interests) { remove_watch(file->epoll_interests); }
  }
}

static unsigned depth(struct open_file *ep, bool parents, struct open_file *forbidden, uint64_t scan) {
  if (ep == forbidden) { return MAX_DEPTH + 1; }
  if (ep->epoll_scan == scan) { return ep->epoll_depth; }
  unsigned longest = 1;
  for (struct epoll_watch *w = parents ? ep->epoll_observers : ep->epoll_interests; w;
       w = parents ? w->observer_next : w->next) {
    struct open_file *child = parents ? w->ep : w->target;
    if (child->type != OPEN_EPOLL) { continue; }
    unsigned n = 1 + depth(child, parents, forbidden, scan);
    if (n > longest) { longest = n; }
    if (longest > MAX_DEPTH) { break; }
  }
  ep->epoll_scan = scan;
  return ep->epoll_depth = longest;
}

int cell_fd_epoll_create(int flags) {
  if (flags & ~CLOEXEC) { return -22; }
  struct open_file *ep = cell_alloc_open_file();
  if (!ep) { return -12; }
  ep->type = OPEN_EPOLL;
  return cell_install_fd(cell_current_domain_internal(), ep, flags & CLOEXEC);
}

int cell_fd_epoll_ctl(int epfd, int op, int fd, uint32_t mask, uint64_t data) {
  struct open_file *ep = cell_fd_file(epfd), *target = cell_fd_file(fd);
  if (!ep || !target) { return -9; }
  if (ep->type != OPEN_EPOLL || ep == target || (op != ADD && op != DEL && op != MOD)) { return -22; }
  if (target->type == OPEN_VFS && target->node.device != RAMFS_DEV_TTY && target->node.device != RAMFS_DEV_CONSOLE) {
    return -1;
  }
  struct epoll_watch *w = target->epoll_observers;
  while (w && (w->ep != ep || w->fd != fd)) { w = w->observer_next; }
  if (op == ADD) {
    if (w) { return -17; }
    if (target->type == OPEN_EPOLL) {
      unsigned below = depth(target, false, ep, ++scan_generation);
      if (below + depth(ep, true, NULL, ++scan_generation) > MAX_DEPTH) { return -40; }
    }
    if (!watches.limit && !pool_init(&watches, sizeof(*w), SIZE_MAX)) { return -12; }
    w = pool_alloc(&watches);
    if (!w) { return -12; }
    w->ep = ep;
    w->target = target;
    w->fd = fd;
    w->link = &ep->epoll_interests;
    w->next = ep->epoll_interests;
    if (w->next) { w->next->link = &w->next; }
    ep->epoll_interests = w;
    w->observer_link = &target->epoll_observers;
    w->observer_next = target->epoll_observers;
    if (w->observer_next) { w->observer_next->observer_link = &w->observer_next; }
    target->epoll_observers = w;
    if (target->type == OPEN_SOCKET) { ++network_watches; }
    attach_sources(w);
  } else if (!w) {
    return -2;
  } else if (op == DEL) {
    remove_watch(w);
    return 0;
  }
  w->mask = mask;
  w->data = data;
  w->disabled = false;
  w->last = 0;
  refresh(w, 0);
  cell_wake_poll_waiters_internal();
  return 0;
}

static int take_ready(struct domain *domain, struct open_file *ep, uint64_t addr, unsigned maxevents) {
  unsigned count = ep->epoll_ready_count < maxevents ? ep->epoll_ready_count : maxevents;
  for (unsigned i = 0; i < count; ++i) {
    struct epoll_watch *w = ep->epoll_ready_head;
    struct epoll_event64 out = {.events = w->pending, .data = w->data};
    if (!vmm_copy_to_user(cell_domain_as(domain), addr + i * sizeof(out), &out, sizeof(out))) {
      return i ? (int)i : -14;
    }
    dequeue(w);
    if (w->mask & ONESHOT) { w->disabled = true; }
    if (w->disabled || (w->mask & EDGE)) { w->pending = 0; }
    else { enqueue(w); }
  }
  if (count && !ep->epoll_ready_head) { cell_epoll_file_changed(ep); }
  return (int)count;
}

void cell_wake_epoll_waiters(void) {
  size_t cursor = 0;
  struct thread *thread;
  while ((thread = cell_next_waiter(WAIT_EPOLL, &cursor))) {
    int n = take_ready(thread->domain, thread->wait.held_file, thread->wait.epoll_events, thread->wait.epoll_maxevents);
    if (n || (thread->wait.has_deadline && cell_uptime_ticks() >= thread->wait.deadline_tick)) { cell_finish_wait(thread, n); }
  }
}

int cell_epoll_wait_current(int epfd, uint64_t addr, int maxevents, int timeout, const uint64_t *mask,
                            struct trap_frame *frame) {
  struct open_file *ep = cell_fd_file(epfd);
  if (!ep) { return -9; }
  if (ep->type != OPEN_EPOLL || maxevents <= 0 || maxevents > INT32_MAX / (int)sizeof(struct epoll_event64)) { return -22; }
  struct domain *domain = cell_current_domain_internal();
  if (!cell_domain_ensure_user_range(domain, addr, (size_t)maxevents * sizeof(struct epoll_event64), VMM_ACCESS_WRITE)) {
    return -14;
  }
  if (network_watches) { net_poll(); }
  cell_epoll_sample_terminals();
  int n = take_ready(domain, ep, addr, (unsigned)maxevents);
  if (n || !timeout || !frame) { return n; }
  struct thread *thread = cell_current_thread_internal();
  cell_retain_open_file(ep);
  thread->wait = (struct thread_wait){.reason = WAIT_EPOLL, .target = epfd, .epoll_events = addr,
                                      .epoll_maxevents = maxevents, .held_file = ep};
  return cell_sleep_poll(thread, frame, mask, timeout > 0, timeout > 0 ? ((uint64_t)timeout + 9) / 10 : 0);
}
