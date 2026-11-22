#pragma once

#include "proc/limits.h"

#include <stdbool.h>
#include <stdint.h>

enum { CELL_FS_RULE_CAP = 8 };

enum cell_fs_right {
  CELL_FS_READ = 1u << 0,
  CELL_FS_WRITE = 1u << 1,
  CELL_FS_EXEC = 1u << 2,
};

enum cell_cap_flag {
  CAP_ENFORCE = 1u << 0,
  CAP_EGRESS_ENFORCE = 1u << 1,
};

struct fs_rule {
  char path[CELL_PATH_MAX];
  uint8_t rights;
};

struct capability_set {
  uint64_t syscall_allow[8];
  uint64_t fs_rights;
  uint64_t flags;
  uint64_t memory_page_cap;
  uint64_t max_domains;
  struct fs_rule fs_rules[CELL_FS_RULE_CAP];
  uint8_t fs_rule_count;
  uint32_t egress_ip;
  uint16_t egress_port;
  uint8_t egress_proto;
  uint8_t egress_prefix;
};

struct cpu_budget {
  uint64_t remaining_ticks;
  uint64_t max_ticks;
};

bool cell_fs_path_allowed(const char *path, uint8_t rights);
bool cell_syscall_allowed(uint64_t nr);
bool cell_egress_allowed(uint8_t proto, uint32_t ip, uint16_t port);
int cell_apply_policy(const char *manifest);
bool cell_map_add_allowed(uint64_t start, uint64_t end, bool replace_existing);
bool cell_map_resize_allowed(uint64_t old_start, uint64_t old_end, uint64_t new_start, uint64_t new_end,
                             bool replace_new);
