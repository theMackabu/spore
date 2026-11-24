#include "abi/linux/shared/fd/handlers.h"
#include "abi/linux/shared/usercopy.h"
#include "internal.h"
#include "mm/pmm.h"
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
  int ready = r->cq_tail != load(&h->cq_head) ? CELL_POLLIN : 0;
  if (load(&h->sq_tail) - r->sq_head < r->entries) { ready |= CELL_POLLOUT; }
  return ready & events;
}

bool uring_submitting;

bool uring_post(struct uring *r, uint64_t data, int result) {
  struct rings *h = pmm_phys_to_virt(r->rings);
  if (r->cq_tail - load(&h->cq_head) >= r->cq_entries) { return false; }
  struct cqe *cq = (void *)(h + 1);
  cq[r->cq_tail & (r->cq_entries - 1)] = (struct cqe){.data = data, .result = result};
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
    if (waiter->wait.held_file == file && r->cq_tail - load(&h->cq_head) >= (unsigned)waiter->wait.target) {
      cell_finish_wait(waiter, waiter->wait.uring_submitted);
    }
  }
  if (file->epoll_observers) { cell_epoll_file_changed(file); }
  if (cell_has_waiters(WAIT_POLL) || cell_has_waiters(WAIT_EPOLL)) { cell_wake_poll_waiters_internal(); }
}

int64_t sys_io_uring_enter(struct trap_frame *frame, int fd, uint32_t submit, uint32_t minimum, uint32_t flags,
                           uint64_t sigmask, uint64_t sigsize) {
  struct open_file *file = uring_file(fd);
  if (!file) { return -EBADF; }
  if (file->ring.disabled) { return -77; }
  if (flags & ~GETEVENTS) { return -EINVAL; }
  uint64_t mask;
  if ((flags & GETEVENTS) && sigmask) {
    if (sigsize != sizeof(mask)) { return -EINVAL; }
    if (!syscall_user_readable(sigmask, sizeof(mask)) ||
        !vmm_copy_from_user(syscall_active_as(), &mask, sigmask, sizeof(mask))) {
      return -EFAULT;
    }
  }
  struct uring *r = &file->ring;
  if (!submit && (!(flags & GETEVENTS) || !minimum)) { return 0; }
  if (!r->rings || (submit && !r->sqes)) { return -EINVAL; }
  struct rings *h = pmm_phys_to_virt(r->rings);
  struct cqe *cq = (void *)(h + 1);
  struct sqe *sq = pmm_phys_to_virt(r->sqes);
  uint32_t pending = load(&h->sq_tail) - r->sq_head;
  if (pending > r->entries) { return -EINVAL; }
  if (submit > pending) { submit = pending; }
  uint32_t head = load(&h->cq_head), start_tail = r->cq_tail;
  uint32_t done = 0;
  bool cancel = false;
  uring_submitting = true;
  while (done < submit) {
    if (r->cq_tail - head >= r->cq_entries) {
      head = load(&h->cq_head);
      if (r->cq_tail - head >= r->cq_entries) { break; }
    }
    uint32_t index = r->sq_head & (r->entries - 1);
    if (r->sq_array_offset) { index = load((uint32_t *)((uint8_t *)h + r->sq_array_offset) + index); }
    ++r->sq_head;
    if (index >= r->entries) {
      publish(&h->dropped, ++r->dropped);
      break;
    }
    struct sqe e = sq[index];
    bool link = e.flags & (IO_LINK | IO_HARDLINK);
    int result;
    if (cancel) {
      result = -125;
    } else if ((e.flags & IO_DRAIN) && r->pending) {
      result = -95;
    } else if (e.opcode == OP_NOP && !e.flags && !e.op_flags && !e.priority && !e.personality) {
      result = 0;
    } else {
      result = uring_issue(file, &e);
    }
    if (result != URING_PENDING && (!(e.flags & CQE_SKIP_SUCCESS) || result < 0)) {
      cq[r->cq_tail++ & (r->cq_entries - 1)] = (struct cqe){e.data, result, 0};
    }
    cancel = link && (cancel || (result < 0 && result != URING_PENDING && !(e.flags & IO_HARDLINK)));
    ++done;
  }
  if (r->cq_tail != start_tail) { publish(&h->cq_tail, r->cq_tail); }
  publish(&h->sq_head, r->sq_head);
  uring_submitting = false;
  if (r->cq_tail != start_tail) { uring_notify(file, false); }
  if (r->pending) { uring_service_events(); }
  if (!done && submit && r->cq_tail - load(&h->cq_head) >= r->cq_entries) { return -EBUSY; }
  if (minimum > r->cq_entries) { minimum = r->cq_entries; }
  if ((flags & GETEVENTS) && minimum > r->cq_tail - load(&h->cq_head)) {
    struct thread *thread = cell_current_thread_internal();
    ++r->waiters;
    cell_retain_open_file(file);
    thread->wait =
      (struct thread_wait){.reason = WAIT_URING, .held_file = file, .target = minimum, .uring_submitted = done};
    return cell_sleep_poll(thread, frame, sigmask ? &mask : NULL, false, 0);
  }
  return done;
}
