#pragma once
#include "abi.h"
#include "io_uring/ring.h"
#include "io_uring/syscalls.h"
#include "proc/fd.h"

struct open_file *uring_file(int fd);
bool uring_opcode_supported(unsigned op);

enum { URING_PENDING = -0x40000001, FIXED_FILE = 1, IO_DRAIN = 2, IO_LINK = 4, IO_HARDLINK = 8, CQE_SKIP_SUCCESS = 64 };
struct domain;
int uring_issue(struct open_file *ring, const struct sqe *e);
int uring_rw(struct open_file *ring, const struct sqe *e);
struct open_file *uring_target(struct uring *ring, const struct sqe *e);
int uring_wait_op(struct open_file *ring, const struct sqe *e);
int uring_defer_io(struct open_file *ring, struct open_file *target, const struct sqe *e);
int uring_retry_io(struct domain *domain, struct open_file *file, const struct sqe *e);
void uring_drop_pending(struct uring *r);
void uring_drop_resources(struct uring *r);
void uring_notify(struct open_file *file, bool async);
bool uring_post(struct uring *r, uint64_t data, int result);
extern bool uring_submitting;

int uring_register_buffers(struct uring *r, uint64_t addr, unsigned nr);
void uring_clear_buffers(struct uring *r);
int uring_fixed_io(struct open_file *ring, const struct sqe *e);
