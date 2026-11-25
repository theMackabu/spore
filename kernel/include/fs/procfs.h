#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct open_file;
struct domain;
struct vfs_node;
struct vfs_dirent;

bool vfs_proc_node_live(const struct vfs_node *node);
struct pid_namespace;
struct mount_access;
struct pid_namespace *vfs_proc_view(const struct mount_access *access);
bool vfs_proc_lookup(const struct mount_access *access, const char *path, bool follow_self, struct vfs_node *out);
bool vfs_proc_readlink(const struct vfs_node *node, char *out, size_t cap, size_t *len_out);
bool vfs_proc_next_dirent(const struct vfs_node *dir, uint64_t *cursor, struct vfs_dirent *out);

int64_t cell_procfs_read_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len, uint64_t offset);
