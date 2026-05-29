#pragma once

#include "cell.h"

bool cell_unix_listener_readable(const struct open_file *listener);
int cell_socket_take_pending_unix(struct domain *domain, struct open_file *listener);
int64_t cell_socket_tcp_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
int64_t cell_socket_tcp_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
bool cell_socket_copy_udp_source_to_domain(struct domain *domain, const struct open_file *file, uint64_t addr,
                                           uint64_t addrlen);
void cell_socket_release_listener(struct open_file *file);
void cell_socket_release_file(struct open_file *file);
void cell_socket_wake_file(struct open_file *file);
