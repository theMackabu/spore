#pragma once

#include "fs/ramfs.h"

extern uint64_t ramfs_now_sec;
void ramfs_reset_pages(uint64_t hhdm_offset);
void ramfs_free_node_pages(struct ramfs *fs, int index);
