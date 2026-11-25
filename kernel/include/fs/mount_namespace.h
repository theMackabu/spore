#pragma once

#include "fs/vfs.h"
struct domain;
struct thread;
struct mount_namespace;
uint64_t mountns_ino(const struct mount_namespace *ns);

constexpr uint64_t CLONE_NEWNS = 0x00020000;
struct mount_namespace *mountns_retain(struct mount_namespace *ns);
void mountns_release(struct mount_namespace *ns);
int mountns_unshare(struct thread *thread);
bool mountns_has_mounts(void);
bool mountns_may_mount(void);
struct devpts_options {
  unsigned mode, ptmxmode;
};
int mountns_proc(const char *target, unsigned flags);
int mountns_memory(const char *path, unsigned mode, unsigned flags, const struct devpts_options *pts);
int mountns_unmount(const char *path, bool detach);
struct ramfs_route {
  struct ramfs *fs;
  const char *path;
  uint64_t mount_id;
  int root;
  const struct vfs_mount *boot;
  struct mount_access *access;
};
struct ramfs_route mountns_lookup(const char *path);
void mountns_access_put(struct mount_access *access);
int mountns_remount(const char *path, unsigned flags);
bool mountns_current_routes(void);
bool mountns_covered_root(const struct vfs_node *node);
int mountns_unstack(bool detach);
int mountns_pivot(const char *root, const char *old);
bool mountns_explicit(const struct domain *domain);
int mountns_relocate_dir(const struct vfs_node *node, const char *opened, char *path, size_t cap);
int mountns_bind(const char *source, const char *target, bool recursive);
const char *mountns_root_path(const struct vfs_mount *mount, char *buf, size_t size);
void mountns_put_fs(struct ramfs *fs);
const struct vfs_mount *vfs_domain_mount_at(const struct domain *domain, size_t index);
const struct vfs_mount *mountns_at(const struct domain *domain, size_t index);
bool mountns_pinned_path(const char *path);
