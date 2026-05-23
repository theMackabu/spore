#pragma once

#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ramfs_file {
    const char *path;
    const void *data;
    uint64_t size;
};

struct ramfs {
    const struct limine_module_response *modules;
};

void ramfs_init(struct ramfs *fs, const struct limine_module_response *modules);
bool ramfs_lookup(const struct ramfs *fs, const char *path, struct ramfs_file *out);

