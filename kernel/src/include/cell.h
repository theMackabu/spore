#pragma once

#include "arch/aarch64/regs.h"
#include "elf/loader.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "ramfs.h"

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
    CELL_SWITCHED = -0x40000000,
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
    WAIT_THREAD,
    WAIT_FUTEX,
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
};

struct open_file {
    bool used;
    uint16_t refcount;
    enum open_file_type type;
    uint64_t offset;
    uint32_t flags;
    struct ramfs_node node;
    uint32_t udp_remote_ip;
    uint16_t udp_local_port;
    uint16_t udp_remote_port;
    bool udp_connected;
    uint8_t udp_rx[256];
    uint64_t udp_rx_len;
};

struct capability_set {
    uint64_t syscall_allow[8];
    uint64_t fs_rights;
    uint64_t flags;
    uint64_t memory_page_cap;
    uint64_t max_domains;
    uint32_t egress_ip;
    uint16_t egress_port;
    uint8_t egress_proto;
    uint8_t egress_prefix;
};

struct cpu_budget {
    uint64_t remaining_ticks;
    uint64_t max_ticks;
};

struct domain {
    int id;
    int parent_id;
    uint16_t refcount;
    bool used;
    bool zombie;
    int exit_status;
    struct user_address_space as;
    struct vma_list vmas;
    struct open_file *fds[MAX_FDS];
    char cwd[128];
    char fs_root[128];
    struct capability_set caps;
    struct cpu_budget budget;
};

struct thread {
    int tid;
    struct domain *domain;
    enum thread_state state;
    struct trap_frame tf;
    uint64_t tpidr_el0;
    enum wait_reason wait_reason;
    int wait_target;
    uint64_t stdin_buf;
    uint64_t stdin_len;
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

void cell_system_init(uint64_t hhdm_offset);
bool cell_create_init(struct user_address_space *as, uint64_t entry, uint64_t sp);
struct user_address_space *cell_current_as(void);
int cell_current_pid(void);
int cell_current_tid(void);
int cell_current_ppid(void);
const char *cell_current_cwd(void);
bool cell_set_cwd(const char *path);
const char *cell_current_fs_root(void);
bool cell_syscall_allowed(uint64_t nr);
bool cell_egress_allowed(uint8_t proto, uint32_t ip, uint16_t port);
int cell_apply_policy(const char *manifest);
bool cell_mmap_allowed(uint64_t pages);
void cell_save_current(const struct trap_frame *frame);
void cell_restore_current(struct trap_frame *frame);
void cell_schedule(struct trap_frame *frame);
void cell_exit_thread_current(int status, struct trap_frame *frame);
void cell_exit_group_current(int status, struct trap_frame *frame);
int cell_fork_current(struct trap_frame *frame);
int cell_clone_thread_current(struct trap_frame *frame,
                              uint64_t flags,
                              uint64_t newsp,
                              uint64_t parent_tid,
                              uint64_t tls,
                              uint64_t child_tid);
int cell_set_tid_address_current(uint64_t clear_child_tid);
int cell_set_robust_list_current(uint64_t robust_list);
int cell_futex_wait_current(uint64_t uaddr, uint32_t expected, struct trap_frame *frame);
int cell_futex_wake_current(uint64_t uaddr, uint32_t count);
int cell_wait4(int pid, uint64_t status_addr, struct trap_frame *frame);
int cell_kill(int pid, int signal);
bool cell_exec_replace(struct user_address_space *as,
                       struct vma_list *vmas,
                       uint64_t entry,
                       uint64_t sp,
                       struct trap_frame *frame);
int64_t cell_fd_write(int fd, uint64_t buf, uint64_t len);
int64_t cell_fd_read(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame);
int64_t cell_fd_lseek(int fd, int64_t off, int whence);
int cell_fd_open_node(const struct ramfs_node *node, uint32_t flags);
int cell_fd_socket_udp(void);
bool cell_fd_udp_bind(int fd, uint16_t port);
bool cell_fd_udp_connect(int fd, uint32_t ip, uint16_t port);
int64_t cell_fd_udp_send(int fd, uint32_t ip, uint16_t port, uint64_t buf, uint64_t len);
int64_t cell_fd_udp_recv(int fd, uint64_t buf, uint64_t len);
int cell_fd_dup(int oldfd, int minfd);
int cell_fd_close(int fd);
bool cell_fd_stat(int fd, struct ramfs_node *out);
bool cell_fd_is_dir(int fd);
bool cell_fd_next_dirent(int fd, struct ramfs_dirent *out);
void cell_fd_rewind_one_dirent(int fd);
uint64_t cell_fd_dir_offset(int fd);
bool cell_handle_cow_fault(uint64_t far);
bool cell_handle_translation_fault(uint64_t far, enum vmm_access access);
bool cell_ensure_user_range(uint64_t va, size_t len, enum vmm_access access);
bool cell_add_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags);
bool cell_remove_vma(uint64_t start, uint64_t end);
bool cell_protect_vma(uint64_t start, uint64_t end, uint32_t prot);
size_t cell_resident_pages(uint64_t start, uint64_t end);
int cell_set_budget(int domain_id, uint64_t ticks);
void cell_timer_tick(struct trap_frame *frame, bool from_lower_el);
void cell_wake_stdin(void);
int snapshot_create_current(void);
int snapshot_spawn(int snap_id, uint64_t entry, uint64_t arg, struct trap_frame *frame);
int snapshot_reap(int pid, uint64_t status_addr, struct trap_frame *frame);
