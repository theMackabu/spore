#pragma once

#include <stdbool.h>
#include <stdint.h>

struct trap_frame;

int64_t sys_pivot_root(uint64_t root, uint64_t old);
int64_t sys_mount(uint64_t source, uint64_t target, uint64_t type, uint64_t flags, uint64_t data);
int64_t sys_umount2(uint64_t target, uint64_t flags);
int64_t sys_inotify_init1(uint64_t flags);
int64_t sys_inotify_add_watch(uint64_t fd, uint64_t path_addr, uint64_t mask);
int64_t sys_inotify_rm_watch(uint64_t fd, uint64_t wd);
int64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t len);
int64_t sys_getcwd(uint64_t buf, uint64_t len);
int64_t sys_chdir(uint64_t path_addr);
int64_t sys_chroot(uint64_t path_addr);
int64_t sys_mkdirat(uint64_t dirfd, uint64_t path_addr, uint64_t mode);
int64_t sys_mknodat(uint64_t dirfd, uint64_t path_addr, uint64_t mode);
int64_t sys_unlinkat(uint64_t dirfd, uint64_t path_addr, uint64_t flags);
int64_t sys_renameat(uint64_t old_dirfd, uint64_t old_path_addr, uint64_t new_dirfd, uint64_t new_path_addr,
                     uint64_t flags);
int64_t sys_linkat(uint64_t old_dirfd, uint64_t old_path_addr, uint64_t new_dirfd, uint64_t new_path_addr,
                   uint64_t flags);
int64_t sys_symlinkat(uint64_t target_addr, uint64_t new_dirfd, uint64_t link_path_addr);
int64_t sys_readlinkat(uint64_t dirfd, uint64_t path_addr, uint64_t buf, uint64_t len);
int64_t sys_utimensat(uint64_t dirfd, uint64_t path_addr, uint64_t times_addr, uint64_t flags);
int64_t sys_fchmodat(uint64_t dirfd, uint64_t path_addr, uint64_t mode, uint64_t flags);
int64_t sys_fchmod(uint64_t fd, uint64_t mode);
int64_t sys_fchownat(uint64_t dirfd, uint64_t path_addr, uint64_t uid_arg, uint64_t gid_arg, uint64_t flags);
int64_t sys_fchown(uint64_t fd, uint64_t uid_arg, uint64_t gid_arg);
int64_t sys_ftruncate(uint64_t fd, uint64_t size);
int64_t sys_fchdir(uint64_t fd);
int64_t sys_fsync(uint64_t fd);
int64_t sys_spore_fsinfo(uint64_t addr);
int64_t sys_spore_mountinfo(uint64_t addr, uint64_t bytes);
int64_t sys_faccessat(uint64_t dirfd, uint64_t path_addr, uint64_t mode, uint64_t flags);
int64_t sys_memfd_create(uint64_t name_addr, uint64_t flags);
int64_t sys_openat(struct trap_frame *frame, uint64_t dirfd, uint64_t path_addr, uint64_t flags, uint64_t mode);
int64_t sys_listxattr(uint64_t path_addr, bool nofollow);
int64_t sys_flistxattr(uint64_t fd);
int64_t sys_fstat(uint64_t fd, uint64_t stat_addr);
int64_t sys_statfs(uint64_t path_addr, uint64_t statfs_addr);
int64_t sys_fstatfs(uint64_t fd, uint64_t statfs_addr);
int64_t sys_statx(uint64_t dirfd, uint64_t path_addr, uint64_t flags, uint64_t mask, uint64_t statx_addr);
int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_addr, uint64_t stat_addr, uint64_t flags);
