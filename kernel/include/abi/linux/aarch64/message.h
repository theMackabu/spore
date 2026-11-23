#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct domain;

struct socket_user_message {
  uint64_t iov;
  int32_t iovlen;
  uint64_t name;
  uint64_t namelen_addr;
};

int aarch64_read_socket_message(struct domain *domain, uint64_t addr, struct socket_user_message *out);
int aarch64_write_msg_flags(struct domain *domain, uint64_t addr, int32_t flags);
int aarch64_iov_capacity(struct domain *domain, uint64_t iov_addr, int32_t iovlen, uint64_t *capacity);
int aarch64_gather_iov_at(struct domain *domain, uint64_t iov_addr, int32_t iovlen, uint64_t offset, uint8_t *dst,
                          size_t len);
int aarch64_scatter_iov(struct domain *domain, uint64_t iov_addr, int32_t iovlen, const uint8_t *src, uint64_t len);
int aarch64_recv_ip_control(struct domain *domain, uint64_t msg_addr, bool emit_ttl, int ttl_value, int32_t *flags);
