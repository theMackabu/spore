#include "abi/linux/shared/iovec.h"
#include "abi/linux/shared/layout.h"
#include "abi/linux/shared/time/handlers.h"
#include "abi/linux/shared/usercopy.h"
#include "internal.h"
#include "mm/pool.h"
#include "mm/table.h"
#include "proc/domain.h"
#include "proc/events/poll_internal.h"
#include "proc/memory.h"

struct uring_pending {
  struct uring_pending *next, *prev, *parent, *child, *timer;
  struct open_file *target;
  struct uring_extra *tag;
  struct iovec64 *iov;
  struct uring_buffer *buffer;
  struct sqe sqe;
  uint64_t context, deadline;
  uint32_t first_completion;
  uint32_t cflags, last_ready;
  uint64_t interval;
  int pid, tid, result;
  bool finished;
};

static struct open_file *active;
static struct object_pool requests;
static bool servicing;

static void deactivate(struct uring *r) {
  if (!r->active_link) { return; }
  *r->active_link = r->next_active;
  if (r->next_active) { r->next_active->ring.active_link = r->active_link; }
  r->active_link = NULL;
  r->next_active = NULL;
}

static int defer(struct open_file *owner, struct open_file *target, const struct sqe *e, uint64_t deadline) {
  struct uring *r = &owner->ring;
  if (r->pending >= 65536) { return -11; }
  if (!requests.limit) { pool_init(&requests, sizeof(struct uring_pending), 65536); }
  struct uring_pending *p = pool_alloc(&requests);
  if (!p) { return -ENOMEM; }
  struct domain *domain = cell_current_domain_internal();
  *p = (struct uring_pending){.prev = r->last,
                              .target = target,
                              .sqe = *e,
                              .deadline = deadline,
                              .pid = domain->id,
                              .tid = cell_current_tid(),
                              .context = cell_domain_as(domain)->context_id,
                              .first_completion = r->cq_tail - r->timeout_cqes};
  if (e->opcode == OP_READV || e->opcode == OP_WRITEV) {
    p->iov = table_alloc(e->len, sizeof(*p->iov));
    if (!p->iov || !vmm_copy_from_user(cell_domain_as(domain), p->iov, e->addr, e->len * sizeof(*p->iov))) {
      table_free(p->iov, e->len, sizeof(*p->iov));
      pool_free(&requests, p);
      return -ENOMEM;
    }
  }
  if (e->opcode == OP_READ_FIXED || e->opcode == OP_WRITE_FIXED) { p->buffer = uring_hold_buffer(r, e->buf_index); }
  if ((e->flags & FIXED_FILE) && target) {
    p->tag = r->files[e->fd].tag;
    if (p->tag) { ++p->tag->refs; }
  }
  p->parent = r->dependency;
  if (p->parent) {
    if (e->opcode == OP_LINK_TIMEOUT) {
      p->parent->timer = p;
    } else {
      p->parent->child = p;
    }
  }
  r->new_request = p;
  cell_retain_open_file(target);
  if (r->last) {
    r->last->next = p;
  } else {
    r->requests = p;
  }
  r->last = p;
  if (e->flags & IO_DRAIN) { ++r->drains; }
  ++r->pending;
  if (!r->active_link) {
    r->next_active = active;
    r->active_link = &active;
    if (active) { active->ring.active_link = &r->next_active; }
    active = owner;
  }
  return URING_PENDING;
}

int uring_defer_io(struct open_file *owner, struct open_file *target, const struct sqe *e) {
  return defer(owner, target, e, 0);
}

static uint64_t now_ns(void) {
  struct timespec64 t = syscall_wall_time(false);
  return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

enum { CANCEL_ALL = 1, CANCEL_FD = 2, CANCEL_ANY = 4, CANCEL_FD_FIXED = 8, CANCEL_USERDATA = 16, CANCEL_OPCODE = 32 };

int uring_cancel(struct uring *r, uint64_t key, int fd, unsigned flags, unsigned opcode) {
  if (flags & ~(CANCEL_ALL | CANCEL_FD | CANCEL_ANY | CANCEL_FD_FIXED | CANCEL_USERDATA | CANCEL_OPCODE)) {
    return -EINVAL;
  }
  if ((flags & CANCEL_ANY) && (flags & (CANCEL_FD | CANCEL_OPCODE | CANCEL_USERDATA))) { return -EINVAL; }
  struct open_file *match = NULL;
  if (flags & CANCEL_FD) {
    struct sqe e = {.fd = fd, .flags = (flags & CANCEL_FD_FIXED) ? FIXED_FILE : 0};
    match = uring_target(r, &e);
    if (!match) { return -EBADF; }
  }
  bool all = flags & (CANCEL_ALL | CANCEL_ANY);
  bool user = (flags & CANCEL_USERDATA) || !(flags & (CANCEL_FD | CANCEL_ANY | CANCEL_OPCODE));
  unsigned found = 0;
  for (struct uring_pending *p = r->requests; p; p = p->next) {
    if (p->finished || (user && p->sqe.data != key) || (match && p->target != match) ||
        ((flags & CANCEL_OPCODE) && p->sqe.opcode != opcode)) {
      continue;
    }
    p->finished = true;
    p->result = -125;
    if (!all) { return 0; }
    ++found;
  }
  return all ? (int)found : -2;
}

static int deadline(uint64_t address, unsigned flags, uint64_t *end) {
  struct timespec64 ts;
  if (uring_copy_in(&ts, address, sizeof(ts))) { return -EFAULT; }
  if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000 ||
      (uint64_t)ts.tv_sec > (uint64_t)(INT64_MAX - ts.tv_nsec) / 1000000000) {
    return -EINVAL;
  }
  uint64_t value = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
  uint64_t now = now_ns();
  if (flags & 1) {
    if (flags & 8) {
      struct timespec64 real = syscall_wall_time(true);
      uint64_t absolute = (uint64_t)real.tv_sec * 1000000000 + real.tv_nsec;
      value = value > absolute ? value - absolute : 0;
    } else {
      *end = value;
      return 0;
    }
  }
  *end = value > UINT64_MAX - now ? UINT64_MAX : now + value;
  return 0;
}

static int update_deadline(struct uring *r, uint64_t key, uint64_t end) {
  for (struct uring_pending *p = r->requests; p; p = p->next) {
    if (!p->finished && p->sqe.opcode == OP_TIMEOUT && p->sqe.data == key) {
      if (p->interval) {
        uint64_t now = now_ns();
        p->interval = end > now ? end - now : 1;
      }
      p->deadline = end;
      return 0;
    }
  }
  return -2;
}

int uring_wait_op(struct open_file *owner, const struct sqe *e) {
  if (e->opcode == OP_POLL_ADD) {
    if ((e->len & ~9u) || e->op_flags & ~0xffffu) { return -95; }
    struct open_file *f = uring_target(&owner->ring, e);
    if (!f) { return -EBADF; }
    if (f->type == OPEN_URING) { return -95; }
    int ready = cell_file_poll_events(f, e->op_flags);
    return ready && !(e->len & 1) && !uring_ordered(&owner->ring, e) ? ready : defer(owner, f, e, 0);
  }
  if (e->flags & FIXED_FILE) { return -EINVAL; }
  if (e->opcode == OP_TIMEOUT || e->opcode == OP_LINK_TIMEOUT) {
    if (e->opcode == OP_LINK_TIMEOUT) {
      if (!owner->ring.linked || e->off || (e->flags & (IO_LINK | IO_HARDLINK))) { return -EINVAL; }
      if (!owner->ring.dependency) { return -125; }
    }
    if (e->len != 1 || e->off > UINT32_MAX || (e->op_flags & ~(1u | 4u | 8u | 32u | 64u)) || (e->op_flags & 12) == 12) {
      return -EINVAL;
    }
    uint64_t end;
    bool multi = e->op_flags & 64;
    if (multi && ((e->op_flags & 1) || e->opcode != OP_TIMEOUT || (e->flags & (IO_LINK | IO_HARDLINK)))) {
      return -EINVAL;
    }
    uint64_t start = multi ? now_ns() : 0;
    int rc = deadline(e->addr, e->op_flags, &end);
    if (rc) { return rc; }
    rc = defer(owner, NULL, e, end);
    if (rc == URING_PENDING && multi) { owner->ring.new_request->interval = end > start ? end - start : 1; }
    return rc;
  }
  if (e->opcode == OP_ASYNC_CANCEL) { return uring_cancel(&owner->ring, e->addr, e->fd, e->op_flags, e->len); }
  if (e->opcode == OP_TIMEOUT_REMOVE && (e->op_flags & 2)) {
    if (e->op_flags & ~(1u | 2u | 4u | 8u)) { return -EINVAL; }
    uint64_t end;
    int rc = deadline(e->off, e->op_flags, &end);
    if (rc) { return rc; }
    if ((e->flags & IO_ASYNC) || uring_ordered(&owner->ring, e)) { return defer(owner, NULL, e, end); }
    return update_deadline(&owner->ring, e->addr, end);
  }
  if (e->op_flags || e->len) { return -EINVAL; }
  for (struct uring_pending *p = owner->ring.requests; p; p = p->next) {
    if (p->finished || p->sqe.data != e->addr) { continue; }
    if (e->opcode == OP_POLL_REMOVE && p->sqe.opcode != OP_POLL_ADD) { continue; }
    if (e->opcode == OP_TIMEOUT_REMOVE && p->sqe.opcode != OP_TIMEOUT) { continue; }
    p->finished = true;
    p->result = -125;
    return 0;
  }
  return -2;
}

static void discard(struct uring *r, struct uring_pending **slot) {
  struct uring_pending *p = *slot;
  *slot = p->next;
  if (p->next) {
    p->next->prev = p->prev;
  } else {
    r->last = p->prev;
  }
  if (p->sqe.flags & IO_DRAIN) { --r->drains; }
  if (p->child) {
    p->child->parent = NULL;
    if (p->result < 0 && !(p->sqe.flags & IO_HARDLINK) &&
        !(p->sqe.opcode == OP_TIMEOUT && p->result == -62 && (p->sqe.op_flags & 32))) {
      p->child->finished = true;
      p->child->result = -125;
    }
  }
  if (p->timer) {
    p->timer->parent = NULL;
    p->timer->finished = true;
    p->timer->result = -125;
  }
  if (p->parent) {
    if (p->sqe.opcode == OP_LINK_TIMEOUT) {
      p->parent->timer = NULL;
    } else {
      p->parent->child = NULL;
    }
  }
  if (!--r->pending) { deactivate(r); }
  cell_release_open_file(p->target);
  uring_put_buffer(r, p->buffer, true);
  table_free(p->iov, p->sqe.len, sizeof(*p->iov));
  uring_tag_put(r, p->tag);
  pool_free(&requests, p);
}

void uring_drop_pending(struct uring *r) {
  deactivate(r);
  while (r->requests) {
    discard(r, &r->requests);
  }
}

static int check(struct uring_pending *p, struct uring *r) {
  if (p->sqe.opcode == OP_LINK_TIMEOUT) {
    if (now_ns() < p->deadline) { return URING_PENDING; }
    if (p->parent && !p->parent->finished) {
      p->parent->finished = true;
      p->parent->result = -125;
      return -62;
    }
    return -125;
  }
  if (p->parent) { return URING_PENDING; }
  if (p->sqe.opcode == OP_TIMEOUT_REMOVE) { return update_deadline(r, p->sqe.addr, p->deadline); }
  if (p->sqe.opcode == OP_REMOVE_BUFFERS || p->sqe.opcode == OP_PROVIDE_BUFFERS) { return uring_group_op(r, &p->sqe); }
  if (p->sqe.opcode == OP_NOP) { return p->sqe.op_flags ? (int)p->sqe.len : 0; }
  if (p->sqe.opcode == OP_TIMEOUT) {
    if (!(p->sqe.op_flags & 64) && p->sqe.off &&
        (uint32_t)(r->cq_tail - r->timeout_cqes - p->first_completion) >= p->sqe.off) {
      return 0;
    }
    return now_ns() >= p->deadline ? -62 : URING_PENDING;
  }
  if (p->sqe.opcode == OP_POLL_ADD) {
    int rc = p->target->type == OPEN_SIGNALFD
               ? cell_thread_poll_events(cell_find_thread(p->tid), p->target, p->sqe.op_flags)
               : cell_file_poll_events(p->target, p->sqe.op_flags);
    if ((p->sqe.len & 1) && !(p->sqe.len & 8)) {
      unsigned previous = p->last_ready;
      p->last_ready = rc;
      if (!(rc & ~previous)) { return URING_PENDING; }
    }
    return rc ? rc : URING_PENDING;
  }
  struct domain *d = cell_find_domain(p->pid);
  if (!d || d->zombie || !d->mm || cell_domain_as(d)->context_id != p->context) { return -125; }
  if (p->buffer) { return uring_fixed_transfer(d, p->buffer, p->target, &p->sqe); }
  if (p->sqe.flags & BUFFER_SELECT) {
    int n = uring_selected_io(r, d, p->target, &p->sqe, &p->cflags);
    return n == -11 ? URING_PENDING : n;
  }
  if (p->sqe.opcode == OP_ACCEPT || p->sqe.opcode == OP_CONNECT || p->sqe.opcode == OP_SENDMSG ||
      p->sqe.opcode == OP_RECVMSG) {
    int rc = uring_retry_socket(d, p->target, &p->sqe);
    return rc == -11 ? URING_PENDING : rc;
  }
  int rc = p->iov ? uring_retry_vector(d, p->target, &p->sqe, p->iov) : uring_retry_io(d, p->target, &p->sqe);
  return rc == -11 ? URING_PENDING : rc;
}

void uring_service_events(void) {
  if (!active || servicing || uring_submitting) { return; }
  servicing = true;
  struct open_file *file = active;
  while (file) {
    struct uring *r = &file->ring;
    struct open_file *next = r->next_active;
    cell_retain_open_file(file);
    bool posted = false;
    struct uring_pending **slot = &r->requests;
    while (*slot) {
      struct uring_pending *p = *slot;
      if ((p->sqe.flags & IO_DRAIN) && p->prev && !p->finished) { break; }
      if (!p->finished) {
        p->result = check(p, r);
        p->finished = p->result != URING_PENDING;
      }
      if (p->finished) {
        bool skip = (p->sqe.flags & CQE_SKIP_SUCCESS) && p->result >= 0;
        bool timer_more = p->sqe.opcode == OP_TIMEOUT && p->interval && p->result == -62 && p->sqe.off != 1;
        bool more = timer_more || (p->sqe.opcode == OP_POLL_ADD && (p->sqe.len & 1) && p->result > 0) ||
                    (p->sqe.opcode == OP_ACCEPT && (p->sqe.priority & 1) && p->result >= 0) ||
                    (p->sqe.opcode == OP_RECV && (p->sqe.priority & 2) && p->result > 0);
        bool published =
          !skip && !r->overflow_head && uring_post(r, p->sqe.data, p->result, p->cflags | (more ? 2u : 0));
        if (!skip && !published) {
          struct uring_extra *extra = uring_extra_new(p->sqe.data);
          if (extra) {
            more = false;
            extra->cqe.result = p->result;
            extra->cqe.flags = p->cflags;
            uring_extra_append(r, extra);
            published = true;
          }
        }
        if (skip || published) {
          posted |= !skip;
          if (!skip && (p->sqe.opcode == OP_TIMEOUT || p->sqe.opcode == OP_LINK_TIMEOUT)) { ++r->timeout_cqes; }
          if (more) {
            p->finished = false;
            p->cflags = 0;
            if (timer_more) {
              if (p->sqe.off) { --p->sqe.off; }
              p->deadline += p->interval;
            }
            slot = &p->next;
            continue;
          }
          discard(r, slot);
          continue;
        }
      }
      if (p->sqe.flags & IO_DRAIN) { break; }
      slot = &p->next;
    }
    unsigned before = r->cq_tail;
    uring_flush_overflow(r);
    if (posted || before != r->cq_tail) { uring_notify(file, true); }
    cell_release_open_file(file);
    file = next;
  }
  servicing = false;
}
