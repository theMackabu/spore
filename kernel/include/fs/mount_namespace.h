#pragma once

#include "fs/vfs.h"
struct domain;
struct thread;
struct mount_namespace;

constexpr uint64_t CLONE_NEWNS = 0x00020000;
struct mount_namespace *mountns_retain(struct mount_namespace *ns);
void mountns_release(struct mount_namespace *ns);
int mountns_unshare(struct thread *thread);
bool mountns_has_mounts(void);
bool mountns_may_mount(void);
int mountns_tmpfs(const char *path, unsigned mode, bool nosuid, bool nodev);
int mountns_unmount(const char *path, bool detach);
struct ramfs *mountns_lookup(const char *path, const char **local);
void mountns_put_fs(struct ramfs *fs);
const struct vfs_mount *vfs_domain_mount_at(const struct domain *domain, size_t index);
const struct vfs_mount *mountns_at(const struct domain *domain, size_t index);
bool mountns_pinned_path(const char *path);
