#pragma once

#include "mm/vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct user_address_space *syscall_active_as(void);
struct vfs_node;
bool syscall_user_readable(uint64_t buf, uint64_t len);
bool syscall_user_writable(uint64_t buf, uint64_t len);
void syscall_realtime_base(uint64_t *epoch_sec, uint64_t *counter, uint64_t *freq);
bool syscall_copy_string_from_user(uint64_t user, char *dst, size_t cap);
bool syscall_normalize_path(const char *base, const char *path, char *out, size_t cap);
bool syscall_copy_resolved_path(uint64_t path_addr, char *out, size_t cap);
int64_t syscall_copy_resolved_path_at(uint64_t dirfd, uint64_t path_addr, char *out, size_t cap);
bool syscall_copy_virtual_path(uint64_t path_addr, char *out, size_t cap);
int64_t syscall_copy_virtual_path_at(uint64_t dirfd, uint64_t path_addr, char *out, size_t cap);
bool syscall_path_policy_denied(void);
bool syscall_node_access_allowed(const struct vfs_node *node, uint64_t mask);
uint8_t syscall_fs_rights_from_access(uint64_t access);

int64_t sys_getrandom(uint64_t buf, uint64_t len);
int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp);
int64_t sys_clock_getres(uint64_t clk_id, uint64_t tp);
int64_t sys_gettimeofday(uint64_t tv_addr, uint64_t tz_addr);
int64_t sys_times(uint64_t buf_addr);
int64_t sys_getrusage(int who, uint64_t usage_addr);

int64_t sys_brk(uint64_t requested);
int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off);
int64_t sys_munmap(uint64_t addr, uint64_t len);
int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot);
int64_t sys_madvise(uint64_t addr, uint64_t len, uint64_t advice);
int64_t sys_mremap(uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint64_t flags, uint64_t new_addr);

int64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_limit, uint64_t old_limit);
int64_t sys_uname(uint64_t buf);
int64_t sys_sethostname(uint64_t name_addr, uint64_t len);
int64_t sys_sysinfo(uint64_t info_addr);
int64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_spore_net_config(uint64_t op, uint64_t cfg_addr);
int64_t sys_sched_getaffinity(uint64_t mask, uint64_t len);

struct trap_frame;

int64_t sys_read(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len);
int64_t sys_write(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len);
int64_t sys_pread64(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t off);
int64_t sys_pwrite64(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t off);
int64_t sys_readv(struct trap_frame *frame, uint64_t fd, uint64_t iov, uint64_t iovcnt);
int64_t sys_writev(struct trap_frame *frame, uint64_t fd, uint64_t iov, uint64_t iovcnt);
int64_t sys_openat(uint64_t dirfd, uint64_t path_addr, uint64_t flags);
int64_t sys_fstat(uint64_t fd, uint64_t stat_addr);
int64_t sys_statfs(uint64_t path_addr, uint64_t statfs_addr);
int64_t sys_fstatfs(uint64_t fd, uint64_t statfs_addr);
int64_t sys_statx(uint64_t dirfd, uint64_t path_addr, uint64_t flags, uint64_t mask, uint64_t statx_addr);
int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_addr, uint64_t stat_addr, uint64_t flags);
int64_t sys_faccessat(uint64_t dirfd, uint64_t path_addr, uint64_t mode, uint64_t flags);
int64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t len);
int64_t sys_getcwd(uint64_t buf, uint64_t len);
int64_t sys_chdir(uint64_t path_addr);
int64_t sys_chroot(uint64_t path_addr);
int64_t sys_mkdirat(uint64_t dirfd, uint64_t path_addr);
int64_t sys_mknodat(uint64_t dirfd, uint64_t path_addr, uint64_t mode);
int64_t sys_unlinkat(uint64_t dirfd, uint64_t path_addr);
int64_t sys_renameat(uint64_t old_dirfd, uint64_t old_path_addr, uint64_t new_dirfd, uint64_t new_path_addr);
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
int64_t sys_execve(struct trap_frame *frame, uint64_t path_addr, uint64_t argv_addr, uint64_t envp_addr);
int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg);
int64_t sys_nanosleep(struct trap_frame *frame, uint64_t req_addr, uint64_t rem_addr);
int64_t sys_clock_nanosleep(struct trap_frame *frame, uint64_t clock_id, uint64_t flags, uint64_t req_addr,
                            uint64_t rem_addr);
int64_t sys_ppoll(struct trap_frame *frame, uint64_t fds, uint64_t nfds, uint64_t timeout_addr, uint64_t sigmask,
                  uint64_t sigsetsize);
int64_t sys_pselect6(struct trap_frame *frame, uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds,
                     uint64_t timeout_addr);
int64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t event_addr);
int64_t sys_epoll_pwait(struct trap_frame *frame, uint64_t epfd, uint64_t events_addr, uint64_t maxevents,
                        uint64_t timeout_ms, uint64_t sigmask, uint64_t sigsetsize);
int64_t sys_sigaltstack(uint64_t new_addr, uint64_t old_addr);
int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset, uint64_t sigsetsize);
int64_t sys_clone(struct trap_frame *frame, uint64_t flags, uint64_t newsp, uint64_t parent_tid, uint64_t tls,
                  uint64_t child_tid);
int64_t sys_futex(struct trap_frame *frame, uint64_t uaddr, uint64_t op, uint64_t val, uint64_t timeout);

int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol);
int64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len);
int64_t sys_connect(struct trap_frame *frame, uint64_t fd, uint64_t addr, uint64_t len);
int64_t sys_listen(uint64_t fd, uint64_t backlog);
int64_t sys_accept(struct trap_frame *frame, uint64_t fd, uint64_t addr, uint64_t addrlen);
int64_t sys_sendto(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr,
                   uint64_t addrlen);
int64_t sys_recvfrom(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr,
                     uint64_t addrlen);
int64_t sys_sendmsg(struct trap_frame *frame, uint64_t fd, uint64_t msg_addr, uint64_t flags);
int64_t sys_recvmsg(struct trap_frame *frame, uint64_t fd, uint64_t msg_addr, uint64_t flags);
int64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen);
int64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen_addr);
