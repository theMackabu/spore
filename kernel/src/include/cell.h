#pragma once

#include "arch/aarch64/regs.h"
#include "elf/loader.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "ramfs.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  MAX_DOMAINS = 16,
  MAX_THREADS = 16,
  MAX_CELLS = MAX_THREADS,
  MAX_SNAPSHOTS = 8,
  MAX_FDS = 32,
  MAX_OPEN_FILES = 64,
  CELL_MAX_POLL_FDS = 64,
  CELL_SWITCHED = -0x40000000,
};

enum cell_poll_events {
  CELL_POLLIN = 0x0001,
  CELL_POLLOUT = 0x0004,
  CELL_POLLERR = 0x0008,
  CELL_POLLHUP = 0x0010,
  CELL_POLLNVAL = 0x0020,
};

enum thread_state {
  THREAD_UNUSED,
  THREAD_RUNNABLE,
  THREAD_BLOCKED,
  THREAD_ZOMBIE,
};

enum wait_reason {
  WAIT_NONE,
  WAIT_CHILD,
  WAIT_STDIN,
  WAIT_SOCKET,
  WAIT_THREAD,
  WAIT_FUTEX,
  WAIT_POLL,
  WAIT_SLEEP,
  WAIT_VFORK,
  WAIT_PIPE,
  WAIT_EPOLL,
};

enum cell_state {
  CELL_UNUSED = THREAD_UNUSED,
  CELL_RUNNABLE = THREAD_RUNNABLE,
  CELL_BLOCKED = THREAD_BLOCKED,
  CELL_ZOMBIE = THREAD_ZOMBIE,
};

enum open_file_type {
  OPEN_NONE,
  OPEN_STDIN,
  OPEN_STDOUT,
  OPEN_RAMFS,
  OPEN_SOCKET,
  OPEN_PIPE,
  OPEN_UNIX_STREAM,
  OPEN_UNIX_LISTENER,
  OPEN_EPOLL,
  OPEN_EVENTFD,
};

enum { CELL_EPOLL_WATCH_CAP = 16 };
enum { CELL_FS_RULE_CAP = 8 };

enum cell_fs_right {
  CELL_FS_READ = 1u << 0,
  CELL_FS_WRITE = 1u << 1,
  CELL_FS_EXEC = 1u << 2,
};

struct fs_rule {
  char path[128];
  uint8_t rights;
};

struct epoll_watch {
  bool used;
  int32_t fd;
  uint32_t events;
  uint64_t data;
};

struct open_file {
  bool used;
  uint16_t refcount;
  enum open_file_type type;
  uint64_t offset;
  uint32_t flags;
  uint8_t pipe_id;
  bool pipe_write_end;
  uint8_t unix_rx_pipe;
  uint8_t unix_tx_pipe;
  int unix_owner_pid;
  uint32_t unix_owner_uid;
  uint32_t unix_owner_gid;
  int unix_peer_pid;
  uint32_t unix_peer_uid;
  uint32_t unix_peer_gid;
  char unix_path[108];
  struct vfs_node node;
  uint8_t socket_proto;
  uint32_t udp_remote_ip;
  uint32_t udp_rx_ip;
  uint16_t udp_local_port;
  uint16_t udp_remote_port;
  uint16_t udp_rx_port;
  bool udp_connected;
  uint8_t udp_rx[1472];
  uint64_t udp_rx_len;
  uint32_t tcp_remote_ip;
  uint32_t tcp_seq;
  uint32_t tcp_ack;
  uint16_t tcp_local_port;
  uint16_t tcp_remote_port;
  uint16_t tcp_rx_len;
  uint8_t tcp_state;
  uint8_t tcp_error;
  bool tcp_fin;
  uint8_t tcp_rx[4096];
  struct epoll_watch epoll_watches[CELL_EPOLL_WATCH_CAP];
  uint64_t eventfd_value;
  bool eventfd_semaphore;
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

struct signal_action {
  uint64_t handler;
  uint64_t flags;
  uint64_t restorer;
  uint64_t mask;
};

struct fp_state {
  uint8_t q[32][16];
  uint64_t fpcr;
  uint64_t fpsr;
} __attribute__((aligned(16)));

struct domain {
  int id;
  int parent_id;
  int pgrp_id;
  int session_id;
  uint16_t refcount;
  bool used;
  bool zombie;
  int exit_status;
  int term_signal;
  uint32_t uid;
  uint32_t euid;
  uint32_t gid;
  uint32_t egid;
  uint64_t start_ticks;
  uint64_t cpu_ticks;
  struct user_address_space as;
  struct vma_list vmas;
  struct open_file *fds[MAX_FDS];
  char name[32];
  char exec_path[128];
  char argv0[64];
  char cmdline[160];
  char cwd[128];
  char fs_root[128];
  char chroot[128];
  struct capability_set caps;
  struct cpu_budget budget;
  struct signal_action signal_actions[65];
};

struct thread {
  int tid;
  struct domain *domain;
  enum thread_state state;
  struct trap_frame tf;
  struct fp_state fp;
  uint64_t tpidr_el0;
  enum wait_reason wait_reason;
  int wait_target;
  uint64_t stdin_buf;
  uint64_t stdin_len;
  uint64_t pipe_buf;
  uint64_t pipe_len;
  bool pipe_write;
  uint8_t poll_kind;
  bool poll_has_deadline;
  uint64_t poll_deadline_tick;
  uint64_t sleep_deadline_tick;
  uint64_t poll_fds;
  uint64_t poll_nfds;
  uint64_t poll_readfds;
  uint64_t poll_writefds;
  uint64_t poll_exceptfds;
  int epoll_fd;
  uint64_t epoll_events;
  int epoll_maxevents;
  uint64_t clear_child_tid;
  uint64_t robust_list;
  uint64_t futex_addr;
};

struct snapshot {
  bool used;
  int id;
  struct user_address_space as;
  struct vma_list vmas;
};

struct proc_info {
  uint32_t pid;
  uint32_t tid;
  uint32_t ppid;
  uint32_t state;
  uint32_t wait_reason;
  uint32_t _pad;
  uint64_t resident_pages;
  uint64_t cpu_ticks;
  uint64_t start_ticks;
  uint64_t remaining_ticks;
  uint64_t max_ticks;
  char name[32];
  char exec_path[128];
  char argv0[64];
  char cmdline[160];
  char cwd[64];
};

struct cell_peer_cred {
  int pid;
  uint32_t uid;
  uint32_t gid;
};

void cell_system_init(uint64_t hhdm_offset);
bool cell_create_init(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t sp);
struct user_address_space *cell_current_as(void);
int cell_current_pid(void);
int cell_current_tid(void);
int cell_current_ppid(void);
uint32_t cell_current_uid(void);
uint32_t cell_current_euid(void);
uint32_t cell_current_gid(void);
uint32_t cell_current_egid(void);
int cell_setuid_current(uint32_t uid);
int cell_setgid_current(uint32_t gid);
void cell_apply_exec_creds(uint32_t mode, uint32_t uid, uint32_t gid);
const char *cell_current_cwd(void);
bool cell_set_cwd(const char *path);
const char *cell_current_fs_root(void);
const char *cell_current_chroot(void);
bool cell_set_chroot(const char *path);
bool cell_fs_path_allowed(const char *path, uint8_t rights);
bool cell_syscall_allowed(uint64_t nr);
bool cell_egress_allowed(uint8_t proto, uint32_t ip, uint16_t port);
int cell_apply_policy(const char *manifest);
bool cell_mmap_allowed(uint64_t pages);
void cell_save_current(const struct trap_frame *frame);
void cell_restore_current(struct trap_frame *frame);
void cell_schedule(struct trap_frame *frame);
void cell_exit_thread_current(int status, struct trap_frame *frame);
void cell_exit_group_current(int status, struct trap_frame *frame);
void cell_signal_current(int signal, struct trap_frame *frame);
int cell_rt_sigaction(int signal, uint64_t act_addr, uint64_t old_addr, uint64_t sigset_size);
int cell_rt_sigreturn(struct trap_frame *frame);
void cell_dump_current_fault(uint64_t esr, uint64_t elr, uint64_t far);
int cell_fork_current(struct trap_frame *frame);
int cell_vfork_current(struct trap_frame *frame);
int cell_clone_thread_current(struct trap_frame *frame, uint64_t flags, uint64_t newsp, uint64_t parent_tid,
                              uint64_t tls, uint64_t child_tid);
int cell_getpgid(int pid);
int cell_setpgid(int pid, int pgid);
int cell_getsid(int pid);
int cell_setsid_current(void);
int cell_tty_foreground_pgrp(void);
int cell_tty_set_foreground_pgrp(int pgid);
int cell_set_tid_address_current(uint64_t clear_child_tid);
int cell_set_robust_list_current(uint64_t robust_list);
int cell_futex_wait_current(uint64_t uaddr, uint32_t expected, struct trap_frame *frame);
int cell_futex_wake_current(uint64_t uaddr, uint32_t count);
int cell_wait4(int pid, uint64_t status_addr, struct trap_frame *frame);
int cell_wait4_options(int pid, uint64_t status_addr, int options, struct trap_frame *frame);
int cell_kill(int pid, int signal);
int cell_tkill(int tid, int signal);
int cell_tgkill(int pid, int tid, int signal);
bool cell_exec_replace(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t sp,
                       struct trap_frame *frame, const char *path, const char *const argv[], uint64_t argc);
bool cell_proc_exists(int pid);
int cell_proc_pid_at(size_t index);
uint32_t cell_proc_uid(int pid);
uint32_t cell_proc_gid(int pid);
int64_t cell_fd_write(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame);
int64_t cell_fd_read(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame);
int cell_fd_poll_events(int fd, int events);
int cell_ppoll_current(uint64_t fds, uint64_t nfds, bool has_timeout, uint64_t timeout_ticks, struct trap_frame *frame);
int cell_pselect6_current(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, bool has_timeout,
                          uint64_t timeout_ticks, struct trap_frame *frame);
int cell_sleep_current(uint64_t timeout_ticks, struct trap_frame *frame);
int64_t cell_fd_pread_kernel(int fd, uint64_t off, void *buf, uint64_t len);
int64_t cell_fd_lseek(int fd, int64_t off, int whence);
int cell_fd_open_node(const struct vfs_node *node, uint32_t flags);
int cell_fd_socket_inet(uint8_t proto);
int cell_fd_socket_unix(void);
bool cell_fd_udp_bind(int fd, uint16_t port);
bool cell_fd_udp_connect(int fd, uint32_t ip, uint16_t port);
int64_t cell_fd_udp_send(int fd, uint32_t ip, uint16_t port, uint64_t buf, uint64_t len);
int cell_fd_tcp_connect(int fd, uint32_t ip, uint16_t port, struct trap_frame *frame);
int64_t cell_fd_tcp_send(int fd, uint64_t buf, uint64_t len);
int64_t cell_fd_socket_recv(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame);
int cell_fd_unix_bind(int fd, const char *path);
int cell_fd_unix_listen(int fd, int backlog);
int cell_fd_unix_accept(int fd, struct trap_frame *frame);
int cell_fd_unix_connect(int fd, const char *path);
bool cell_fd_unix_peer_cred(int fd, struct cell_peer_cred *out);
void cell_net_deliver_udp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const void *payload, size_t len);
void cell_net_deliver_tcp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack,
                          uint8_t flags, const void *payload, size_t len);
void cell_net_deliver_icmp(uint32_t src_ip, const void *payload, size_t len);
int cell_fd_pipe2(uint64_t pipefd_addr, int flags);
int cell_fd_epoll_create(int flags);
int cell_fd_epoll_ctl(int epfd, int op, int fd, uint32_t events, uint64_t data);
int cell_fd_epoll_wait(int epfd, uint64_t events_addr, int maxevents);
int cell_epoll_wait_current(int epfd, uint64_t events_addr, int maxevents, int timeout_ms, struct trap_frame *frame);
int cell_fd_eventfd(uint64_t initval, int flags);
int cell_fd_dup(int oldfd, int minfd);
int cell_fd_dup3(int oldfd, int newfd, int flags);
int cell_fd_get_flags(int fd);
int cell_fd_set_flags(int fd, int flags);
int cell_fd_get_fd_flags(int fd);
int cell_fd_set_fd_flags(int fd, int flags);
int cell_fd_close(int fd);
bool cell_fd_stat(int fd, struct vfs_node *out);
bool cell_fd_is_dir(int fd);
bool cell_fd_next_dirent(int fd, struct vfs_dirent *out);
void cell_fd_rewind_one_dirent(int fd);
uint64_t cell_fd_dir_offset(int fd);
void cell_fd_set_dir_offset(int fd, uint64_t offset);
bool cell_handle_cow_fault(uint64_t far);
bool cell_handle_translation_fault(uint64_t far, enum vmm_access access);
bool cell_ensure_user_range(uint64_t va, size_t len, enum vmm_access access);
bool cell_vma_overlaps(uint64_t start, uint64_t end);
bool cell_vma_lookup_range(uint64_t start, uint64_t end, struct vma *out);
bool cell_add_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags);
bool cell_add_vma_typed(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, enum vma_type type);
bool cell_add_file_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, const struct vfs_node *node,
                       uint64_t file_start, uint64_t file_offset, uint64_t file_size);
bool cell_remove_vma(uint64_t start, uint64_t end);
bool cell_protect_vma(uint64_t start, uint64_t end, uint32_t prot);
size_t cell_resident_pages(uint64_t start, uint64_t end);
size_t cell_proc_info(struct proc_info *out, size_t max);
uint32_t cell_tty_lflag(void);
void cell_tty_set_lflag(uint32_t lflag);
uint8_t cell_tty_erase_char(void);
void cell_tty_set_erase_char(uint8_t ch);
int cell_set_budget(int domain_id, uint64_t ticks);
void cell_timer_tick(struct trap_frame *frame, bool from_lower_el);
void cell_set_boot_epoch(uint64_t epoch_sec);
void cell_wake_stdin(struct trap_frame *frame);
int snapshot_create_current(void);
int snapshot_spawn(int snap_id, uint64_t entry, uint64_t arg, struct trap_frame *frame);
int snapshot_reap(int pid, uint64_t status_addr, struct trap_frame *frame);
