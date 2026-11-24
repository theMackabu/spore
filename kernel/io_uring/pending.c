#include "abi/linux/shared/layout.h"
#include "abi/linux/shared/time/handlers.h"
#include "abi/linux/shared/usercopy.h"
#include "internal.h"
#include "mm/pool.h"
#include "proc/domain.h"
#include "proc/events/poll_internal.h"
#include "proc/memory.h"

struct uring_pending {
  struct uring_pending *next;
  struct open_file *target;
  struct sqe sqe;
  uint64_t context, deadline;
  uint32_t first_completion;
  int pid, result;
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
  if (e->flags & (IO_LINK | IO_HARDLINK)) { return -95; }
  if (r->pending >= r->entries) { return -11; }
  if (!requests.limit) { pool_init(&requests, sizeof(struct uring_pending), 65536); }
  struct uring_pending *p = pool_alloc(&requests);
  if (!p) { return -ENOMEM; }
  struct domain *domain = cell_current_domain_internal();
  *p = (struct uring_pending){.next = r->requests,
                              .target = target,
                              .sqe = *e,
                              .deadline = deadline,
                              .pid = domain->id,
                              .context = cell_domain_as(domain)->context_id,
                              .first_completion = r->cq_tail};
  cell_retain_open_file(target);
  r->requests = p;
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

int uring_wait_op(struct open_file *owner, const struct sqe *e) {
  if (e->opcode == OP_POLL_ADD) {
    if (e->len || e->op_flags & ~0xffffu) { return -95; }
    struct open_file *f = uring_target(&owner->ring, e);
    if (!f) { return -EBADF; }
    if (f->type == OPEN_URING) { return -95; }
    int ready = cell_file_poll_events(f, e->op_flags);
    return ready ? ready : defer(owner, f, e, 0);
  }
  if (e->flags & FIXED_FILE) { return -EINVAL; }
  if (e->opcode == OP_TIMEOUT) {
    struct timespec64 ts;
    if (e->len != 1 || e->off > UINT32_MAX || (e->op_flags & ~(1u | 32u))) { return -EINVAL; }
    if (!syscall_user_readable(e->addr, sizeof(ts)) ||
        !vmm_copy_from_user(syscall_active_as(), &ts, e->addr, sizeof(ts))) {
      return -EFAULT;
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000 ||
        (uint64_t)ts.tv_sec > (uint64_t)(INT64_MAX - ts.tv_nsec) / 1000000000u) {
      return -EINVAL;
    }
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    uint64_t base = e->op_flags & 1 ? 0 : now_ns();
    return defer(owner, NULL, e, ns > UINT64_MAX - base ? UINT64_MAX : base + ns);
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
  if (!--r->pending) { deactivate(r); }
  cell_release_open_file(p->target);
  pool_free(&requests, p);
}

void uring_drop_pending(struct uring *r) {
  deactivate(r);
  while (r->requests) {
    discard(r, &r->requests);
  }
}

static int check(struct uring_pending *p, struct uring *r) {
  if (p->sqe.opcode == OP_TIMEOUT) {
    if (p->sqe.off && (uint32_t)(r->cq_tail - p->first_completion) >= p->sqe.off) { return 0; }
    return now_ns() >= p->deadline ? (p->sqe.op_flags & 32 ? 0 : -62) : URING_PENDING;
  }
  if (p->sqe.opcode == OP_POLL_ADD) {
    int rc = cell_file_poll_events(p->target, p->sqe.op_flags);
    return rc ? rc : URING_PENDING;
  }
  struct domain *d = cell_find_domain(p->pid);
  if (!d || d->zombie || !d->mm || cell_domain_as(d)->context_id != p->context) { return -125; }
  int rc = uring_retry_io(d, p->target, &p->sqe);
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
      if (!p->finished) {
        p->result = check(p, r);
        p->finished = p->result != URING_PENDING;
      }
      if (p->finished) {
        bool skip = (p->sqe.flags & CQE_SKIP_SUCCESS) && p->result >= 0;
        if (skip || uring_post(r, p->sqe.data, p->result)) {
          posted |= !skip;
          discard(r, slot);
          continue;
        }
      }
      slot = &p->next;
    }
    if (posted) { uring_notify(file, true); }
    cell_release_open_file(file);
    file = next;
  }
  servicing = false;
}
