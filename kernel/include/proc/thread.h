#pragma once

#include "arch/regs.h"
#include "proc/capabilities.h"
#include "proc/limits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct domain;
struct open_file;
struct unix_rights;

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
  WAIT_FIFO_OPEN,
  WAIT_EPOLL,
  WAIT_INOTIFY,
  WAIT_PTY,
  WAIT_FLOCK,
  WAIT_EVENTFD,
  WAIT_SIGNALFD,
  WAIT_SIGWAIT,
  WAIT_URING,
  WAIT_REASON_COUNT,
};

constexpr int CELL_SWITCHED = -0x40000000;

struct thread_wait {
  enum wait_reason reason;
  int target;
  uint64_t deadline_tick;
  bool has_deadline;
  bool restore_signal_mask;
  struct open_file *held_file;
  union {
    struct {
      uint64_t buf;
      uint64_t len;
      uint64_t socket_addr;
      uint64_t socket_addrlen;
      uint64_t socket_msg_addr;
      struct unix_rights *unix_rights;
      uint64_t socket_iov;
      uint64_t socket_iovlen;
      uint32_t socket_flags;
      int socket_accept_flags;
      bool write;
      bool socket_msg, socket_vector;
      bool socket_servicing;
    };
    struct {
      uint64_t saved_signal_mask;
      uint64_t poll_fds;
      uint64_t poll_nfds;
      uint64_t poll_readfds;
      uint64_t poll_writefds;
      uint64_t poll_exceptfds;
      uint64_t epoll_events;
      int epoll_maxevents;
      uint8_t poll_kind;
      uint32_t uring_submitted, uring_timeouts;
    };
    struct {
      uint64_t sigwait_set, sigwait_info;
    };
    bool fifo_cloexec;
    struct {
      uint64_t futex_addr;
      uint32_t futex_bitset;
    };
    struct {
      uint64_t child_status, child_usage;
      bool child_servicing;
    };
    struct {
      union {
        uint8_t flock_mode;
        struct {
          uint64_t event_arg;
          bool event_write;
        };
      };
    };
  };
};

struct thread {
  struct pid_entry *pid_entry;
  int tid;
  int running_cpu;
  uint64_t cpu_ticks, minor_faults, major_faults;
  uint64_t start_ticks;
  struct domain *domain;
  enum thread_state state;
  struct trap_frame tf;
  struct arch_thread_context context;
  struct thread_wait wait;
  struct linux_caps capabilities;
  bool no_new_privs;
  uint64_t signal_mask;
  uint8_t parent_death_signal;
  uint64_t pending_signals;
  uint64_t sigaltstack_sp;
  uint64_t sigaltstack_size;
  uint32_t sigaltstack_flags;
  uint64_t clear_child_tid;
  uint64_t robust_list;
};

bool cell_thread_reset(size_t capacity);
size_t cell_thread_slot_limit(void);
void cell_set_current_thread(struct thread *thread);
bool cell_scheduler_waiting_for_interrupt(void);
void cell_wait_reset(void);
bool cell_has_waiters(enum wait_reason reason);
struct thread *cell_next_waiter(enum wait_reason reason, size_t *cursor);
void cell_mark_blocked(struct thread *thread);
void cell_clear_wait(struct thread *thread);
void cell_resume_wait(struct thread *thread);
void cell_finish_wait(struct thread *thread, int64_t result);
int cell_block_current(struct trap_frame *frame, struct thread_wait wait);
void cell_release_thread(struct thread *thread);
size_t cell_runnable_or_blocked_threads_in_domain(const struct domain *domain);
void cell_wake_vfork_parent_of(int child_id);
int cell_block_current_on_sleep(uint64_t deadline_tick, struct trap_frame *frame);
int cell_block_current_on_pipe(int fd, uint64_t buf, uint64_t len, bool write, struct trap_frame *frame);
int cell_block_current_on_socket_accept(int fd, uint64_t addr, uint64_t addrlen, int flags, struct trap_frame *frame);
int cell_block_current_on_socket_flags_timeout(int fd, uint64_t buf, uint64_t len, uint64_t addr, uint64_t addrlen,
                                               uint32_t flags, uint64_t timeout_ticks, struct trap_frame *frame);
int cell_block_current_on_socket_write_timeout(int fd, uint64_t buf, uint64_t len, uint64_t timeout_ticks,
                                               struct trap_frame *frame);
int cell_block_current_on_socket_sendmsg_timeout(int fd, uint64_t msg_addr, uint64_t timeout_ticks,
                                                 struct trap_frame *frame);
int cell_block_current_on_socket_msg_timeout(int fd, uint64_t msg_addr, uint64_t iov, uint64_t iovlen, uint64_t addr,
                                             uint64_t addrlen, uint32_t flags, uint64_t timeout_ticks,
                                             struct trap_frame *frame);
void cell_save_current(const struct trap_frame *frame);
void cell_timer_tick(struct trap_frame *frame, bool from_lower_el);
void cell_schedule(struct trap_frame *frame);
#ifdef __wasm__
void cell_resume_scheduler(struct trap_frame *frame);
#endif
int cell_sleep_current(uint64_t timeout_ticks, struct trap_frame *frame);
struct thread *cell_current_thread_internal(void);
struct thread *cell_thread_slot(size_t index);
struct thread *cell_alloc_thread(struct domain *domain, bool leader);
struct thread *cell_find_thread(int tid);
struct thread *cell_thread_for_domain(const struct domain *domain);

int cell_current_tid(void);
