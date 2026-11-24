#include "abi/linux/shared/fd/handlers.h"
#include "abi/linux/shared/layout.h"
#include "abi/linux/shared/usercopy.h"
#include "internal.h"
#include "mm/pmm.h"
#include "proc/accounting.h"
#include "proc/domain.h"
#include "proc/epoll.h"
#include "proc/eventfd.h"
#include "proc/events/poll_internal.h"
#include "proc/poll.h"

static uint32_t load(const uint32_t *v) {
  return __atomic_load_n(v, __ATOMIC_ACQUIRE);
}
static void publish(uint32_t *v, uint32_t n) {
  __atomic_store_n(v, n, __ATOMIC_RELEASE);
}

int uring_poll(struct uring *r, int events) {
  if (!r->rings) { return events & CELL_POLLOUT; }
  struct rings *h = pmm_phys_to_virt(r->rings);
  int ready = (r->overflow_head || r->cq_tail != load(&h->cq_head)) ? CELL_POLLIN : 0;
  if (load(&h->sq_tail) - r->sq_head < r->entries) { ready |= CELL_POLLOUT; }
  return ready & events;
}

bool uring_submitting;

static inline void write_cqe(struct cqe *cq, unsigned tail, unsigned mask, unsigned shift, struct cqe c) {
  unsigned slot = (tail & mask) << shift;
  cq[slot] = c;
  if (shift) { cq[slot + 1] = (struct cqe){0}; }
}

static unsigned copy_nops(const struct sqe *sq, struct cqe *cq, unsigned max) {
  unsigned n = 0;
  for (; n < max; ++n) {
    uint32_t opcode_flags_priority;
    __builtin_memcpy(&opcode_flags_priority, &sq[n], sizeof(opcode_flags_priority));
    if (opcode_flags_priority || sq[n].personality || sq[n].op_flags) { break; }
    cq[n] = (struct cqe){.data = sq[n].data};
  }
  return n;
}

bool uring_post(struct uring *r, uint64_t data, int result, uint32_t flags) {
  struct rings *h = pmm_phys_to_virt(r->rings);
  if (r->cq_tail - load(&h->cq_head) >= r->cq_entries) { return false; }
  struct cqe *cq = (void *)(h + 1);
  write_cqe(cq, r->cq_tail, r->cq_entries - 1, r->cq_shift,
            (struct cqe){.data = data, .result = result, .flags = flags});
  publish(&h->cq_tail, ++r->cq_tail);
  return true;
}

void uring_notify(struct open_file *file, bool async) {
  struct uring *r = &file->ring;
  struct rings *h = pmm_phys_to_virt(r->rings);
  if (r->eventfd && (!r->eventfd_async || async) && !(load(&h->cq_flags) & 1)) { cell_eventfd_signal(r->eventfd); }
  size_t cursor = 0;
  struct thread *waiter;
  while (r->waiters && (waiter = cell_next_waiter(WAIT_URING, &cursor))) {
    if (waiter->wait.held_file == file && (r->timeout_cqes != waiter->wait.uring_timeouts ||
                                           r->cq_tail - load(&h->cq_head) >= (unsigned)waiter->wait.target)) {
      cell_finish_wait(waiter, waiter->wait.uring_submitted);
    }
  }
  if (file->epoll_observers) { cell_epoll_file_changed(file); }
  if (cell_has_waiters(WAIT_POLL) || cell_has_waiters(WAIT_EPOLL)) { cell_wake_poll_waiters_internal(); }
}

int64_t sys_io_uring_enter(struct trap_frame *frame, int fd, uint32_t submit, uint32_t minimum, uint32_t flags,
                           uint64_t sigmask, uint64_t sigsize) {
  struct open_file *file = uring_file(fd);
  if (!file) { return cell_fd_file(fd) ? -95 : -EBADF; }
  if (file->ring.disabled) { return -77; }
  if (file->ring.issuer && file->ring.issuer != cell_current_tid()) { return -17; }
  if (flags & ~(GETEVENTS | ENTER_EXT_ARG)) { return -EINVAL; }
  bool timed = false;
  uint64_t timeout = 0;
  if (flags & ENTER_EXT_ARG) {
    struct {
      uint64_t mask;
      uint32_t size, reserved;
      uint64_t ts;
    } arg;
    if (sigsize != sizeof(arg)) { return -EINVAL; }
    if (uring_copy_in(&arg, sigmask, sizeof(arg))) { return -EFAULT; }
    if (arg.reserved) { return -EINVAL; }
    sigmask = arg.mask;
    sigsize = arg.size;
    if (arg.ts) {
      struct timespec64 ts;
      if (uring_copy_in(&ts, arg.ts, sizeof(ts))) { return -EFAULT; }
      if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000 || (uint64_t)ts.tv_sec > INT64_MAX / 100) {
        return -EINVAL;
      }
      timed = true;
      timeout = (uint64_t)ts.tv_sec * 100 + (ts.tv_nsec + 9999999) / 10000000;
    }
  }
  uint64_t mask;
  if ((flags & GETEVENTS) && sigmask) {
    if (sigsize != sizeof(mask)) { return -EINVAL; }
    if (!syscall_user_readable(sigmask, sizeof(mask)) ||
        !vmm_copy_from_user(syscall_active_as(), &mask, sigmask, sizeof(mask))) {
      return -EFAULT;
    }
  }
  struct uring *r = &file->ring;
  if (r->overflow_head) {
    unsigned old = r->cq_tail;
    uring_flush_overflow(r);
    if (old != r->cq_tail) { uring_notify(file, false); }
  }
  if (r->pending) { uring_service_events(); }
  if (!submit && (!(flags & GETEVENTS) || !minimum)) { return 0; }
  if (!r->rings || (submit && !r->sqes)) { return -EINVAL; }
  struct rings *h = pmm_phys_to_virt(r->rings);
  struct cqe *cq = (void *)(h + 1);
  struct sqe *sq = pmm_phys_to_virt(r->sqes);
  uint32_t pending = load(&h->sq_tail) - r->sq_head;
  if (pending > r->entries) { return -EINVAL; }
  if (submit > pending) { submit = pending; }
  uint32_t head = load(&h->cq_head), start_tail = r->cq_tail;
  const unsigned sqmask = r->entries - 1, cqmask = r->cq_entries - 1;
  const unsigned sq_shift = r->sq_shift, cq_shift = r->cq_shift;
  const uint32_t *array = r->sq_array_offset ? (const void *)((const char *)h + r->sq_array_offset) : NULL;
  uint32_t done = 0;
  bool cancel = false;
  uring_submitting = true;
  while (done < submit) {
    if (submit - done >= 4 && !array && !sq_shift && !cq_shift && !r->restricted && !r->linked && !r->drains &&
        !r->overflow_head) {
      unsigned used = r->cq_tail - head;
      if (used < r->cq_entries) {
        unsigned si = r->sq_head & sqmask, ci = r->cq_tail & cqmask;
        unsigned n = submit - done;
        if (n > r->entries - si) { n = r->entries - si; }
        if (n > r->cq_entries - ci) { n = r->cq_entries - ci; }
        if (n > r->cq_entries - used) { n = r->cq_entries - used; }
        n = copy_nops(sq + si, cq + ci, n);
        if (n) {
          r->sq_head += n;
          r->cq_tail += n;
          done += n;
          continue;
        }
      }
    }
    struct uring_extra *overflow = NULL;
    if (r->cq_tail - head >= r->cq_entries || r->overflow_head) {
      head = load(&h->cq_head);
      if (r->cq_tail - head >= r->cq_entries || r->overflow_head) {
        if (!(overflow = uring_extra_new(0))) { break; }
      }
    }
    uint32_t index = r->sq_head & sqmask;
    if (array) { index = load(&array[index]); }
    ++r->sq_head;
    if (index >= r->entries) {
      if (overflow) { uring_extra_free(overflow); }
      publish(&h->dropped, ++r->dropped);
      break;
    }
    const struct sqe *entry = &sq[index << sq_shift];
    uint32_t opcode_flags_priority;
    __builtin_memcpy(&opcode_flags_priority, entry, sizeof(opcode_flags_priority));
    if (!opcode_flags_priority && !r->drains && !r->linked && !cancel && !r->restricted && !overflow &&
        !entry->personality && !entry->op_flags) {
      write_cqe(cq, r->cq_tail++, cqmask, cq_shift, (struct cqe){.data = entry->data});
      ++done;
      continue;
    }
    struct sqe e = *entry;
    r->new_request = NULL;
    bool link = e.flags & (IO_LINK | IO_HARDLINK);
    int result;
    uint32_t cflags = 0;
    if (r->restricted && (e.opcode >= 64 || !(r->allowed_ops & (1ull << e.opcode)) || (e.flags & ~r->allowed_flags) ||
                          (e.flags & r->required_flags) != r->required_flags)) {
      result = -13;
    } else if (cancel) {
      result = -125;
    } else {
      result = uring_issue(file, &e, &cflags);
    }
    if (result != URING_PENDING && (!(e.flags & CQE_SKIP_SUCCESS) || result < 0)) {
      struct cqe c = {e.data, result, cflags};
      if (overflow) {
        overflow->cqe = c;
        uring_extra_append(r, overflow);
        overflow = NULL;
      } else {
        write_cqe(cq, r->cq_tail++, cqmask, cq_shift, c);
      }
    }
    if (overflow) { uring_extra_free(overflow); }
    r->dependency = link ? r->new_request : NULL;
    r->linked = link;
    cancel = link && (cancel || (result < 0 && result != URING_PENDING && !(e.flags & IO_HARDLINK)));
    ++done;
  }
  if (r->cq_tail != start_tail) { publish(&h->cq_tail, r->cq_tail); }
  publish(&h->sq_head, r->sq_head);
  r->dependency = NULL;
  r->new_request = NULL;
  r->linked = false;
  uring_submitting = false;
  if (r->cq_tail != start_tail) { uring_notify(file, false); }
  if (r->pending) { uring_service_events(); }
  if (!done && submit && r->cq_tail - load(&h->cq_head) >= r->cq_entries) { return -EBUSY; }
  if (minimum > r->cq_entries) { minimum = r->cq_entries; }
  if ((flags & GETEVENTS) && minimum > r->cq_tail - load(&h->cq_head)) {
    if (timed && !timeout) { return done || r->cq_tail != load(&h->cq_head) ? (int64_t)done : -62; }
    struct thread *thread = cell_current_thread_internal();
    ++r->waiters;
    cell_retain_open_file(file);
    thread->wait = (struct thread_wait){.reason = WAIT_URING,
                                        .held_file = file,
                                        .target = minimum,
                                        .uring_submitted = done,
                                        .uring_timeouts = r->timeout_cqes};
    return cell_sleep_poll(thread, frame, sigmask ? &mask : NULL, timed, timeout);
  }
  return done;
}
