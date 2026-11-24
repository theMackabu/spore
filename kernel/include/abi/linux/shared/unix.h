#pragma once
#include <stdbool.h>
#include <stdint.h>

struct domain;
struct open_file;
struct iovec64;
struct unix_rights;
int64_t linux_unix_transfer(struct domain *domain, struct open_file *file, uint64_t msg_addr, uint32_t flags,
                              bool write, struct unix_rights **rights);
int64_t linux_unix_vector(struct domain *domain, struct open_file *file, uint64_t iov, unsigned count, bool write,
                          const struct iovec64 *snapshot);
