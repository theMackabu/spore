#pragma once

#include "fs/ramfs.h"

extern uint64_t ramfs_now_sec;
void ramfs_reset_pages(uint64_t hhdm_offset);
void ramfs_free_node_pages(struct ramfs *fs, int index);
int ramfs_add_node(struct ramfs *fs, int parent, const char *name, bool is_dir, bool writable);
int ramfs_find_child(const struct ramfs *fs, int parent, const char *name);
void ramfs_populate(struct ramfs *fs, int root, const struct spore_boot_module *modules, uint32_t count);
