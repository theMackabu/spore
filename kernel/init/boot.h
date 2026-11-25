#pragma once

#include "init/boot_info.h"
#include "init/program.h"
#include <stdbool.h>
#include <stddef.h>

struct ext2_fs;
struct ramfs;
struct net_config;

struct boot_profile {
  const struct spore_boot_module *payload;
  struct init_program program;
};

enum boot_error {
  BOOT_INIT_FAILED = 1,
  BOOT_FLUSH_FAILED = 2,
  BOOT_INVALID_PAYLOAD = 4,
  BOOT_DATA_MOUNT_FAILED = 5,
};

void boot_select_profile(const struct spore_boot_info *boot, struct boot_profile *profile);
struct ext2_fs *boot_mount_disk(uint64_t hhdm_offset);
void boot_mount_root(const struct spore_boot_info *boot, struct ext2_fs *disk, struct boot_profile *profile);
bool app_unpack(const void *data, size_t size, struct ramfs *fs, struct init_program *program,
                struct net_config *network);
[[noreturn]] void boot_start_process(const struct init_program *program, uint64_t hhdm_offset);
[[noreturn]] void boot_fail(enum boot_error error, const char *reason);
