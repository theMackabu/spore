#pragma once

#include <stdbool.h>
#include <stdint.h>

struct domain;
struct open_file;

int64_t cell_read_regular(struct domain *domain, struct open_file *file, uint64_t off, uint64_t buf, uint64_t len);
int64_t cell_write_regular(struct domain *domain, struct open_file *file, uint64_t off, uint64_t buf, uint64_t len);
int64_t cell_read_generated(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
int64_t cell_block_device_io(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len, bool write);
