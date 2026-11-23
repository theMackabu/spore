#pragma once

#include "proc/fd.h"
#include "net/socket.h"

enum {
  CELL_O_NONBLOCK = 04000,
  CELL_O_CLOEXEC = 02000000,
  IPPROTO_TCP = 6,
  IPPROTO_UDP = 17,
  IPPROTO_ICMP = 1,
  MSG_PEEK = 0x2,
  MSG_WAITALL = 0x100,
  EPERM = 1,
  EAGAIN = 11,
  EFAULT = 14,
  EINVAL = 22,
  EIO = 5,
  ENOTCONN = 107,
  EPIPE = 32,
  ECONNREFUSED = 111,
  EADDRINUSE = 98,
};

struct sockaddr_in_cell {
  uint16_t sin_family;
  uint16_t sin_port;
  uint32_t sin_addr;
  uint8_t sin_zero[8];
};

enum tcp_socket_state {
  TCP_CLOSED,
  TCP_LISTEN,
  TCP_SYN_SENT,
  TCP_SYN_RECEIVED,
  TCP_ESTABLISHED,
  TCP_FIN_WAIT,
};

void dgram_clear_rx(struct open_file *file);
int64_t dgram_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len, uint64_t addr,
                             uint64_t addrlen, uint32_t flags);
int64_t dgram_read_to_kernel(struct domain *domain, struct open_file *file, void *buf, uint64_t len, uint64_t addr,
                             uint64_t addrlen, uint32_t flags, uint16_t *original_len);
uint32_t tcp_send_chunk_limit(const struct open_file *file);
bool tcp_send_would_block(const struct open_file *file);
int64_t tcp_write_one_from_kernel(struct open_file *file, const void *buf, uint64_t len);
int64_t cell_socket_tcp_read_to_domain_flags(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len,
                                             uint32_t flags);
int64_t cell_socket_tcp_read_to_kernel(struct open_file *file, void *buf, uint64_t len, uint32_t flags);
bool tcp_recv_waitall_pending(const struct open_file *file, uint64_t want, uint32_t flags);
bool tcp_init_socket(struct open_file *file);
void tcp_release_file(struct open_file *file);
int tcp_copy_peer_addr_to_domain(struct domain *domain, int accepted_fd, uint64_t addr, uint64_t addrlen);
void tcp_timer_tick(uint64_t now_ticks);
uint16_t net_bswap16(uint16_t v);
uint32_t socket_effective_buffer(const struct open_file *file, bool receive);
uint8_t socket_effective_ttl(const struct open_file *file);
void socket_apply_accept_flags(struct domain *domain, int fd, int flags);
void socket_close_domain_fd(struct domain *domain, int fd);
void wake_socket_waiters(struct open_file *file);
