#pragma once

#include <stdbool.h>
#include <stdint.h>

struct trap_frame;

enum cell_poll_events {
  CELL_POLLIN = 0x0001,
  CELL_POLLOUT = 0x0004,
  CELL_POLLERR = 0x0008,
  CELL_POLLHUP = 0x0010,
  CELL_POLLNVAL = 0x0020,
};

int cell_ppoll_current(uint64_t fds, uint64_t nfds, bool has_timeout, uint64_t timeout_ticks, const uint64_t *mask, struct trap_frame *frame);
int cell_pselect6_current(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, bool has_timeout,
                          uint64_t timeout_ticks, const uint64_t *mask, struct trap_frame *frame);
int cell_fd_epoll_create(int flags);
int cell_fd_epoll_ctl(int epfd, int op, int fd, uint32_t events, uint64_t data);
int cell_epoll_wait_current(int epfd, uint64_t events_addr, int maxevents, int timeout_ms, const uint64_t *mask, struct trap_frame *frame);
void cell_wake_poll_waiters_internal(void);
