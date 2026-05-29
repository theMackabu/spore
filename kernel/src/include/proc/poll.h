#pragma once

#include "cell.h"

int cell_fd_poll_events(int fd, int events);
int cell_ppoll_current(uint64_t fds, uint64_t nfds, bool has_timeout, uint64_t timeout_ticks, struct trap_frame *frame);
int cell_pselect6_current(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, bool has_timeout,
                          uint64_t timeout_ticks, struct trap_frame *frame);
int cell_fd_epoll_create(int flags);
int cell_fd_epoll_ctl(int epfd, int op, int fd, uint32_t events, uint64_t data);
int cell_fd_epoll_wait(int epfd, uint64_t events_addr, int maxevents);
int cell_epoll_wait_current(int epfd, uint64_t events_addr, int maxevents, int timeout_ms, struct trap_frame *frame);
void cell_wake_poll_waiters_internal(void);
void cell_wake_epoll_waiters_internal(void);
void cell_socket_wake_unix_accept_waiters(struct open_file *listener);
