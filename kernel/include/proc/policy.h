#pragma once

#include <stddef.h>

#include <stdbool.h>
#include <stdint.h>

enum cell_fs_right {
  CELL_FS_READ = 1u << 0,
  CELL_FS_WRITE = 1u << 1,
  CELL_FS_EXEC = 1u << 2,
};

struct capability_set;
bool cell_policy_init(size_t domains);
struct capability_set *cell_policy_retain(struct capability_set *caps);
void cell_policy_release(struct capability_set *caps);

struct cpu_budget {
  uint64_t remaining_ticks;
  uint64_t max_ticks;
};

struct vfs_node;
bool cell_fs_exec_fd_allowed(const char *path, const struct vfs_node *node);
bool cell_fs_path_allowed(const char *path, uint8_t rights);
bool cell_syscall_allowed(uint64_t nr);
bool cell_egress_allowed(uint8_t proto, uint32_t ip, uint16_t port);
int cell_apply_policy(const char *manifest);
bool cell_map_add_allowed(uint64_t start, uint64_t end, bool replace_existing);
bool cell_map_resize_allowed(uint64_t old_start, uint64_t old_end, uint64_t new_start, uint64_t new_end,
                             bool replace_new);
