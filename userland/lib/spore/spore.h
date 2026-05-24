#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

enum exit_code {
  EXIT_USAGE = 64,
};

int eprintf(const char *fmt, ...);
int usage(const char *tool, const char *usage);
const char *basename(const char *path);
bool streq(const char *a, const char *b);

enum {
  SYS_spore_procinfo = 0x4007,
  SYS_spore_fsinfo = 0x4008,
  SYS_spore_mountinfo = 0x4009,
};

struct proc_info {
  uint32_t pid;
  uint32_t tid;
  uint32_t ppid;
  uint32_t state;
  uint32_t wait_reason;
  uint32_t _pad;
  uint64_t resident_pages;
  uint64_t remaining_ticks;
  uint64_t max_ticks;
  char cwd[64];
};

struct fs_info {
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
  uint64_t inode_count;
  uint64_t free_inodes;
};

struct mount_info {
  char source[32];
  char target[32];
  char fstype[16];
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
};
