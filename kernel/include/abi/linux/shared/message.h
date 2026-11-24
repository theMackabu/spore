#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct domain;
struct iovec64;
struct user_address_space;

struct linux_iov_cursor {
  const struct iovec64 *snapshot;
  struct domain *domain;
  struct user_address_space *as;
  uint64_t next, end, addr, remaining, capacity;
};

struct socket_user_message {
  uint64_t iov;
  int32_t iovlen;
  uint64_t name;
  uint64_t namelen_addr;
};

int linux_read_socket_message(struct domain *domain, uint64_t addr, struct socket_user_message *out);
int linux_write_msg_flags(struct domain *domain, uint64_t addr, int32_t flags);
int linux_iov_init(struct linux_iov_cursor *cursor, struct domain *domain, uint64_t iov, int32_t count);
int linux_gather_iov(void *cursor, uint8_t *dst, size_t len);
int linux_scatter_iov(void *cursor, const uint8_t *src, size_t len);
int linux_recv_ip_control(struct domain *domain, uint64_t msg_addr, bool emit_ttl, int ttl_value, int32_t *flags);
int linux_iov_from_snapshot(struct linux_iov_cursor *cursor, struct domain *domain, const struct iovec64 *iov,
                            unsigned count);
