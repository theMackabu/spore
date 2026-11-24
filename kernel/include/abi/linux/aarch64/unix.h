#pragma once
#include <stdbool.h>
#include <stdint.h>

struct domain;
struct open_file;
struct unix_rights;
int64_t aarch64_unix_transfer(struct domain *domain, struct open_file *file, uint64_t msg_addr, uint32_t flags,
                              bool write, struct unix_rights **rights);
