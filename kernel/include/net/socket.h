#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct domain;
struct open_file;
struct pipe_obj;
struct trap_frame;

enum {
  CELL_TCP_RX_CAP = 262144,
  CELL_TCP_TX_CAP = 1460,
  CELL_TCP_TX_QUEUE_CAP = 128,
  CELL_DGRAM_RX_CAP = 1472,
};

struct cell_peer_cred {
  int pid;
  uint32_t uid;
  uint32_t gid;
};

int cell_socket_poll_events(const struct open_file *file, int events);
int64_t cell_socket_tcp_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
void cell_unix_release_file(struct open_file *file);
void cell_socket_release_file(struct open_file *file);
void cell_socket_wake_file(struct open_file *file);
void cell_socket_timer_tick(uint64_t now_ticks);
int cell_fd_socketpair_unix(uint64_t sv_addr, int flags);
int cell_fd_unix_queue_right_range(int fd, int passed_fd, uint64_t start, uint64_t end);
int cell_fd_unix_recv_right_for_range(int fd, uint64_t start, uint64_t end, int fd_flags);
bool cell_fd_unix_tx_offset(int fd, uint64_t *out);
bool cell_fd_unix_rx_offset(int fd, uint64_t *out);
bool cell_fd_unix_next_right_range(int fd, uint64_t *start, uint64_t *end);
void cell_unix_release_rights_for_pipe(struct pipe_obj *pipe);
int cell_fd_socket_inet(uint8_t proto);
int cell_fd_socket_unix(void);
bool cell_fd_is_datagram(int fd);
int64_t cell_fd_dgram_writev(int fd, uint64_t iov, int32_t count);
int64_t cell_fd_dgram_readv(int fd, uint64_t iov, int32_t count, struct trap_frame *frame);
bool cell_fd_socket_info(int fd, int32_t *type, int32_t *proto);
int cell_fd_socket_error(int fd, bool clear);
bool cell_fd_socket_local_addr(int fd, uint32_t *ip, uint16_t *port);
bool cell_fd_socket_peer_addr(int fd, uint32_t *ip, uint16_t *port);
int cell_fd_socket_get_int_option(int fd, int level, int optname, int32_t *out);
int cell_fd_socket_set_int_option(int fd, int level, int optname, int32_t value);
bool cell_fd_socket_get_linger(int fd, bool *enabled, uint32_t *seconds);
bool cell_fd_socket_set_linger(int fd, bool enabled, uint32_t seconds);
bool cell_fd_socket_accepting(int fd, bool *accepting);
bool cell_fd_socket_get_timeout(int fd, bool receive, uint64_t *ticks);
bool cell_fd_socket_set_timeout(int fd, bool receive, uint64_t ticks);
int cell_fd_udp_bind(int fd, uint16_t port);
bool cell_fd_udp_connect(int fd, uint32_t ip, uint16_t port);
int cell_fd_udp_disconnect(int fd);
int64_t cell_fd_udp_send(int fd, uint32_t ip, uint16_t port, uint64_t buf, uint64_t len);
int64_t cell_fd_udp_send_kernel(int fd, uint32_t ip, uint16_t port, const void *buf, uint64_t len);
int cell_fd_tcp_bind(int fd, uint16_t port);
int cell_fd_tcp_listen(int fd, int backlog);
int cell_fd_tcp_accept(int fd, uint64_t addr, uint64_t addrlen, int flags, struct trap_frame *frame);
int cell_fd_tcp_connect(int fd, uint32_t ip, uint16_t port, struct trap_frame *frame);
int64_t cell_fd_tcp_send(int fd, uint64_t buf, uint64_t len, bool dontwait, struct trap_frame *frame);
int cell_fd_socket_shutdown(int fd, int how);
int64_t cell_fd_socket_recv(int fd, uint64_t buf, uint64_t len, uint32_t flags, bool dontwait, struct trap_frame *frame,
                            uint64_t addr, uint64_t addrlen);
int64_t cell_fd_socket_sendmsg(int fd, uint64_t msg_addr, bool dontwait, struct trap_frame *frame);
int64_t cell_fd_socket_recvmsg(int fd, uint64_t msg_addr, uint32_t flags, bool dontwait, struct trap_frame *frame);
int cell_fd_unix_bind(int fd, const char *path);
int cell_fd_unix_listen(int fd, int backlog);
int cell_fd_unix_accept(int fd, int flags, struct trap_frame *frame);
int cell_fd_unix_connect(int fd, const char *path);
bool cell_fd_unix_peer_cred(int fd, struct cell_peer_cred *out);
bool cell_net_deliver_udp(uint32_t src_ip, uint8_t ttl, uint16_t src_port, uint16_t dst_port, const void *payload, size_t len);
bool cell_net_deliver_udp_error(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port, int error);
bool cell_net_deliver_tcp_error(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port, int error);
void cell_net_deliver_tcp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack,
                          uint16_t window, uint8_t flags, const void *options, size_t options_len, const void *payload,
                          size_t len);
void cell_net_deliver_icmp(uint32_t src_ip, uint8_t ttl, const void *payload, size_t len);
