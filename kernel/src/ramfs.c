#include "ramfs.h"

#include "mem.h"

static bool streq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (*a++ != *b++) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

static bool path_matches(const struct limine_file *file, const char *path) {
    if (file->string != NULL && streq(file->string, path)) {
        return true;
    }
    if (file->path == NULL) {
        return false;
    }

    const char *module_path = file->path;
    size_t module_len = kstrlen(module_path);
    size_t target_len = kstrlen(path);
    return module_len >= target_len &&
           streq(module_path + module_len - target_len, path);
}

void ramfs_init(struct ramfs *fs, const struct limine_module_response *modules) {
    fs->modules = modules;
}

bool ramfs_lookup(const struct ramfs *fs, const char *path, struct ramfs_file *out) {
    if (fs->modules == NULL) {
        return false;
    }

    for (uint64_t i = 0; i < fs->modules->module_count; ++i) {
        const struct limine_file *file = fs->modules->modules[i];
        if (!path_matches(file, path)) {
            continue;
        }
        out->path = path;
        out->data = file->address;
        out->size = file->size;
        return true;
    }
    return false;
}

