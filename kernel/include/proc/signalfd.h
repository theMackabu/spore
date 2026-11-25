#pragma once
#include <stdint.h>

struct open_file;
struct thread;
struct trap_frame;
struct domain;

int cell_signalfd(int fd, uint64_t mask, unsigned flags);
int64_t cell_signalfd_read(struct thread *thread, struct open_file *file, uint64_t addr, uint64_t len,
                           struct trap_frame *frame);
int cell_signalfd_ready(const struct thread *thread, const struct open_file *file);
void cell_signalfd_notify(struct domain *domain);
void cell_signalfd_close(void);
