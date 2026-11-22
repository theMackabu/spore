#pragma once

#include <stdint.h>

struct trap_frame;

void cell_snapshot_reset(void);
int snapshot_create_current(void);
int snapshot_spawn(int snap_id, uint64_t entry, uint64_t arg, struct trap_frame *frame);
int snapshot_reap(int pid, uint64_t status_addr, struct trap_frame *frame);
