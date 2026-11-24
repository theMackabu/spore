#pragma once
#include <stdint.h>
struct trap_frame;
unsigned freebsd_errno(unsigned error);
int64_t freebsd_dispatch(struct trap_frame *frame, uint64_t nr, const uint64_t *args);
void freebsd_result(struct trap_frame *frame, int64_t result);
