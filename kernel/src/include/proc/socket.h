#pragma once

#include "cell.h"

bool cell_unix_listener_readable(const struct open_file *listener);
bool cell_tcp_listener_readable(const struct open_file *listener);
bool cell_tcp_socket_writable(const struct open_file *file);
int cell_socket_take_pending_unix(struct domain *domain, struct open_file *listener, int flags);
int cell_socket_take_pending_tcp(struct domain *domain, struct open_file *listener, int flags);
int64_t cell_socket_tcp_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
int64_t cell_socket_tcp_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
bool cell_socket_copy_udp_source_to_domain(struct domain *domain, const struct open_file *file, uint64_t addr,
                                           uint64_t addrlen);
void cell_socket_release_listener(struct open_file *file);
void cell_socket_release_file(struct open_file *file);
void cell_socket_wake_file(struct open_file *file);
void cell_socket_timer_tick(uint64_t now_ticks);
int cell_fd_socketpair_unix(uint64_t sv_addr, int flags);
int cell_fd_unix_queue_right(int fd, int passed_fd);
int cell_fd_unix_recv_right(int fd, int fd_flags);
int cell_fd_unix_queue_right_range(int fd, int passed_fd, uint64_t start, uint64_t end);
int cell_fd_unix_recv_right_for_range(int fd, uint64_t start, uint64_t end, int fd_flags);
bool cell_fd_unix_tx_offset(int fd, uint64_t *out);
bool cell_fd_unix_rx_offset(int fd, uint64_t *out);
bool cell_fd_unix_next_right_range(int fd, uint64_t *start, uint64_t *end);
void cell_unix_release_rights_for_pipe(uint8_t pipe_id);
