#pragma once

#include <stdbool.h>
#include <stdint.h>

struct domain;
struct trap_frame;
struct seccomp_filter;
struct seccomp_filter *seccomp_inherit(struct seccomp_filter *filter);
void seccomp_release(struct seccomp_filter *filter);
int64_t sys_seccomp(unsigned op, unsigned flags, uint64_t address);
bool seccomp_check(struct trap_frame *frame, uint64_t nr, const uint64_t args[6], int64_t *result);
