#pragma once
#include "proc/thread.h"

struct open_file;
int cell_thread_poll_events(struct thread *thread, struct open_file *file, int events);
int cell_file_poll_events(struct open_file *file, int events);
int cell_sleep_poll(struct thread *thread, struct trap_frame *frame, const uint64_t *mask, bool has_timeout,
                    uint64_t timeout_ticks);
void cell_epoll_sample_terminals(void);
void cell_wake_epoll_waiters(void);
