#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct trap_frame;

int64_t cell_fd_write(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame);
int64_t cell_fd_read(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame);
int64_t cell_fd_positioned_io(int fd, uint64_t buf, uint64_t len, int64_t position, bool write);

struct user_address_space;
int64_t cell_read_generated(const struct user_address_space *as, uint64_t buf, size_t len, bool random);
struct open_file;
struct domain;
int64_t cell_file_regular_io(struct domain *domain, struct open_file *file, uint64_t buf, size_t len,
                             uint64_t position, bool write);
