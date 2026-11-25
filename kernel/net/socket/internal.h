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
  MSG_TRUNC = 0x20,
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

static inline void socket_index_insert(struct open_file **bucket, struct open_file *file) {
  file->socket_index_next = *bucket;
  *bucket = file;
}

static inline void socket_index_remove(struct open_file **bucket, struct open_file *file) {
  while (*bucket != NULL) {
    if (*bucket == file) {
      *bucket = file->socket_index_next;
      file->socket_index_next = NULL;
      return;
    }
    bucket = &(*bucket)->socket_index_next;
  }
}

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

int tcp_poll_events(const struct open_file *file, int events);
int dgram_poll_events(const struct open_file *file, int events);
int unix_poll_events(const struct open_file *file, int events);
bool cell_tcp_socket_writable(const struct open_file *file);
uint32_t socket_effective_rcvlowat(const struct open_file *file);

struct socket_datagram {
  struct socket_datagram *next;
  uint32_t ip;
  uint16_t port, len;
  uint8_t ttl;
  char unix_name[109];
  uint8_t data[CELL_DGRAM_RX_CAP];
};

bool dgram_push_from(struct open_file *file, uint32_t ip, uint8_t ttl, uint16_t port, const char *name, const void *data, size_t len);
bool dgram_push_rx(struct open_file *file, uint32_t ip, uint8_t ttl, uint16_t port, const void *data, size_t len);
void dgram_clear_rx(struct open_file *file);
void udp_release_file(struct open_file *file);
typedef int (*socket_copy_out)(void *destination, const uint8_t *src, size_t len);
int64_t dgram_read(struct domain *domain, struct open_file *file, uint64_t len, uint64_t addr, uint64_t addrlen,
                   uint32_t flags, socket_copy_out copy, void *destination);
int64_t socket_sendmsg_tcp_from_domain(struct domain *domain, struct open_file *file, uint64_t msg_addr);
int64_t socket_recvmsg_to_iov(struct domain *domain, struct open_file *file, uint64_t msg_addr, uint64_t iov,
                              int32_t iovlen, uint64_t addr, uint64_t addrlen, uint32_t flags);
bool tcp_send_would_block(const struct open_file *file);
typedef int (*tcp_copy_payload)(void *source, uint8_t *dst, size_t len);
int64_t tcp_write_stream(struct open_file *file, uint64_t len, tcp_copy_payload copy, void *source);
int64_t tcp_read_stream(struct open_file *file, uint64_t len, uint32_t flags, socket_copy_out copy, void *destination);
uint32_t tcp_rx_available(const struct open_file *file);
bool tcp_recv_waitall_pending(const struct open_file *file, uint64_t want, uint32_t flags);
void tcp_init_socket(struct open_file *file);
void tcp_release_file(struct open_file *file);
void tcp_timer_tick(uint64_t now_ticks);
uint16_t net_bswap16(uint16_t v);
uint32_t socket_effective_buffer(const struct open_file *file, bool receive);
uint8_t socket_effective_ttl(const struct open_file *file);
int take_pending_unix(struct domain *domain, struct open_file *listener, int flags);
