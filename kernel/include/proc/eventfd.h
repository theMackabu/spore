#pragma once

#include <stdbool.h>
#include <stdint.h>

struct domain;
struct open_file;
struct trap_frame;

int cell_fd_eventfd(uint32_t initial, int flags);
int64_t cell_eventfd_io(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len, bool write,
                        struct trap_frame *frame);
void cell_eventfd_signal(struct open_file *file);
