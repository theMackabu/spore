#pragma once

#include <stdint.h>

struct trap_frame;

int64_t sys_openat(struct trap_frame *frame, uint64_t dirfd, uint64_t path_addr, uint64_t flags, uint64_t mode);
int64_t sys_fstat(uint64_t fd, uint64_t stat_addr);
int64_t sys_statfs(uint64_t path_addr, uint64_t statfs_addr);
int64_t sys_fstatfs(uint64_t fd, uint64_t statfs_addr);
int64_t sys_statx(uint64_t dirfd, uint64_t path_addr, uint64_t flags, uint64_t mask, uint64_t statx_addr);
int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_addr, uint64_t stat_addr, uint64_t flags);
