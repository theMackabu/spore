#include "ramfs.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    const char init_data[] = "init";
    struct limine_file init = {
        .address = (void *)init_data,
        .size = sizeof(init_data),
        .path = "boot():/boot/init",
        .string = "/init",
    };
    struct limine_file other = {
        .address = (void *)"x",
        .size = 1,
        .path = "boot():/boot/other",
        .string = "/other",
    };
    struct limine_file *files[] = {&other, &init};
    struct limine_module_response modules = {
        .module_count = 2,
        .modules = files,
    };

    struct ramfs fs;
    struct ramfs_file file;
    ramfs_init(&fs, &modules);

    assert(ramfs_lookup(&fs, "/init", &file));
    assert(file.data == init_data);
    assert(file.size == sizeof(init_data));
    assert(strcmp(file.path, "/init") == 0);
    assert(!ramfs_lookup(&fs, "/missing", &file));
    return 0;
}

