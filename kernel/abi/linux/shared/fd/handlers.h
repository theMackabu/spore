#pragma once

#include <stdint.h>
#include "write.h"

struct trap_frame;

int64_t sys_read(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len);
int64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t len, uint64_t off);
int64_t sys_pwrite64(uint64_t fd, uint64_t buf, uint64_t len, uint64_t off);
int64_t sys_readv(struct trap_frame *frame, uint64_t fd, uint64_t iov, uint64_t iovcnt);
int64_t sys_writev(struct trap_frame *frame, uint64_t fd, uint64_t iov, uint64_t iovcnt);
int64_t sys_fcntl(uint64_t fd_num, uint64_t cmd, uint64_t arg);
int64_t sys_close_range(uint64_t first, uint64_t end, uint64_t flags);
int64_t sys_ppoll(struct trap_frame *frame, uint64_t fds, uint64_t nfds, uint64_t timeout_addr, uint64_t sigmask,
                  uint64_t sigsetsize);
int64_t sys_pselect6(struct trap_frame *frame, uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds,
                     uint64_t timeout_addr, uint64_t sigmask_data);
int64_t sys_epoll_pwait(struct trap_frame *frame, uint64_t epfd, uint64_t events_addr, uint64_t maxevents,
                        uint64_t timeout_ms, uint64_t sigmask, uint64_t sigsetsize);
