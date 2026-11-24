#pragma once
#include "abi.h"
#include "io_uring/ring.h"
#include "io_uring/syscalls.h"
#include "proc/fd.h"

struct open_file *uring_file(int fd);
bool uring_opcode_supported(unsigned op);

enum {
  URING_PENDING = -0x40000001,
  FIXED_FILE = 1,
  IO_DRAIN = 2,
  IO_LINK = 4,
  IO_HARDLINK = 8,
  IO_ASYNC = 16,
  BUFFER_SELECT = 32,
  CQE_SKIP_SUCCESS = 64
};
struct domain;
int uring_issue(struct open_file *ring, const struct sqe *e, uint32_t *flags);
int uring_rw(struct open_file *ring, const struct sqe *e);
struct open_file *uring_target(struct uring *ring, const struct sqe *e);
int uring_wait_op(struct open_file *ring, const struct sqe *e);
int uring_defer_io(struct open_file *ring, struct open_file *target, const struct sqe *e);
int uring_retry_io(struct domain *domain, struct open_file *file, const struct sqe *e);
void uring_drop_pending(struct uring *r);
void uring_drop_resources(struct uring *r);
void uring_notify(struct open_file *file, bool async);
bool uring_post(struct uring *r, uint64_t data, int result, uint32_t flags);
extern bool uring_submitting;

int uring_register_buffers(struct uring *r, uint64_t addr, unsigned nr, uint64_t tags, bool sparse);
int uring_update_buffers(struct uring *r, unsigned offset, uint64_t addr, unsigned nr, uint64_t tags);
void uring_clear_buffers(struct uring *r);
int uring_fixed_io(struct open_file *ring, const struct sqe *e);

struct uring_extra {
  struct uring_extra *next;
  struct cqe cqe;
  unsigned refs;
};
struct uring_slot {
  struct open_file *file;
  struct uring_extra *tag;
};
struct uring_extra *uring_extra_new(uint64_t data);
void uring_extra_free(struct uring_extra *item);
void uring_extra_append(struct uring *r, struct uring_extra *item);
void uring_tag_put(struct uring *ring, struct uring_extra *tag);
void uring_flush_overflow(struct uring *ring);
void uring_clear_overflow(struct uring *ring);
int uring_copy_in(void *dst, uint64_t addr, size_t size);
struct iovec64;
int uring_retry_vector(struct domain *d, struct open_file *target, const struct sqe *e, const struct iovec64 *iov);
int uring_socket_op(struct open_file *ring, const struct sqe *e);
int uring_retry_socket(struct domain *d, struct open_file *target, const struct sqe *e);
int uring_cancel(struct uring *r, uint64_t key, int fd, unsigned flags, unsigned opcode);

int uring_group_op(struct uring *r, const struct sqe *e);
int uring_selected_io(struct uring *r, struct domain *d, struct open_file *f, const struct sqe *e, uint32_t *flags);
void uring_drop_groups(struct uring *r);
int uring_register_group(struct uring *r, unsigned op, uint64_t addr, unsigned count);

static inline bool uring_ordered(const struct uring *r, const struct sqe *e) {
  return r->dependency || r->drains || ((e->flags & IO_DRAIN) && r->pending);
}

int uring_update_files(struct uring *r, unsigned offset, uint64_t data, unsigned nr, uint64_t tags, bool update);
int uring_install_direct(struct uring *r, uint32_t index, struct open_file *target);
int uring_direct_result(struct uring *r, uint32_t index, int fd);
int uring_close_direct(struct uring *r, uint32_t index);
int uring_message(struct open_file *source, const struct sqe *e);
bool uring_prepare_rings(struct uring *r);
