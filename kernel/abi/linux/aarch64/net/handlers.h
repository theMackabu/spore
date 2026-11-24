#pragma once

#include <stdint.h>

struct trap_frame;

int64_t sys_spore_net_config(uint64_t op, uint64_t cfg_addr);
int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol);
int64_t sys_socketpair(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t sv);
int64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len);
int64_t sys_connect(struct trap_frame *frame, uint64_t fd, uint64_t addr, uint64_t len);
int64_t sys_listen(uint64_t fd, uint64_t backlog);
int64_t sys_accept(struct trap_frame *frame, uint64_t fd, uint64_t addr, uint64_t addrlen);
int64_t sys_accept4(struct trap_frame *frame, uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t flags);
int64_t sys_sendto(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr,
                   uint64_t addrlen);
int64_t sys_recvfrom(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr,
                     uint64_t addrlen);
int64_t sys_sendmsg(struct trap_frame *frame, uint64_t fd, uint64_t msg_addr, uint64_t flags);
int64_t sys_recvmsg(struct trap_frame *frame, uint64_t fd, uint64_t msg_addr, uint64_t flags);
int64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen);
int64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t addrlen);
int64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen);
int64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen_addr);
int64_t sys_shutdown(uint64_t fd, uint64_t how);
