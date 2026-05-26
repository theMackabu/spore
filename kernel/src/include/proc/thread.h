#pragma once

#include "cell.h"

void cell_thread_reset(void);
struct domain *cell_current_domain_internal(void);
struct thread *cell_current_thread_internal(void);
void cell_set_current_thread(struct thread *thread);
bool cell_scheduler_waiting_for_interrupt(void);
struct thread *cell_thread_slot(size_t index);
size_t cell_thread_index(const struct thread *thread);
struct thread *cell_alloc_thread(struct domain *domain);
void cell_release_thread(struct thread *thread);
struct thread *cell_thread_for_domain(struct domain *domain);
size_t cell_runnable_or_blocked_threads_in_domain(const struct domain *domain);
void cell_wake_vfork_parent_of(int child_id);
void cell_wake_sleep_waiters(uint64_t scheduler_ticks);
int cell_block_current_on_sleep(uint64_t deadline_tick, struct trap_frame *frame);
int cell_block_current_on_pipe(int fd, uint64_t buf, uint64_t len, bool write, struct trap_frame *frame);
int cell_block_current_on_socket(int fd, uint64_t buf, uint64_t len, uint64_t addr, uint64_t addrlen,
                                 struct trap_frame *frame);
void cell_socket_wake_unix_accept_waiters(struct open_file *listener);
void cell_wake_poll_waiters_internal(void);
