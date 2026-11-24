#include "internal.h"
#include "abi/linux/shared/usercopy.h"
#include "abi/linux/shared/fd/handlers.h"
#include "proc/domain.h"
#include "proc/epoll.h"
#include "proc/events/poll_internal.h"
#include "proc/poll.h"
#include "mm/pmm.h"

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

int64_t sys_io_uring_enter(struct trap_frame *frame, int fd, uint32_t submit, uint32_t minimum, uint32_t flags,
                           uint64_t sigmask, uint64_t sigsize, bool allow_epoll_ctl) {
  struct open_file *file = uring_file(fd);
  if (!file) { return -EBADF; }
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
  uint32_t done = 0;
  while (done < submit && r->cq_tail - load(&h->cq_head) < r->cq_entries) {
    uint32_t i = r->sq_head & (r->entries - 1);
    if (r->sq_array_offset) { i = load((uint32_t *)((uint8_t *)h + r->sq_array_offset) + i); }
    ++r->sq_head;
    if (i >= r->entries) {
      publish(&h->dropped, ++r->dropped);
      break;
    }
    struct sqe e = sq[i];
    int result = -EINVAL;
    if (!e.flags && e.opcode == OP_NOP) { result = 0; }
    if (!e.flags && e.opcode == OP_EPOLL_CTL) {
      result = allow_epoll_ctl ? sys_epoll_ctl(e.fd, e.len, e.off, e.addr) : -1;
    }
    cq[r->cq_tail & (r->cq_entries - 1)] = (struct cqe){e.data, result, 0};
    publish(&h->cq_tail, ++r->cq_tail);
    ++done;
  }
  publish(&h->sq_head, r->sq_head);
  if (done) {
    size_t cursor = 0;
    struct thread *waiter;
    while ((waiter = cell_next_waiter(WAIT_URING, &cursor))) {
      if (waiter->wait.held_file == file && r->cq_tail - load(&h->cq_head) >= (unsigned)waiter->wait.target) {
        cell_finish_wait(waiter, waiter->wait.uring_submitted);
      }
    }
    cell_epoll_file_changed(file);
    cell_wake_poll_waiters_internal();
  } else if (submit && r->cq_tail - load(&h->cq_head) >= r->cq_entries) {
    return -EBUSY;
  }
  if (minimum > r->cq_entries) { minimum = r->cq_entries; }
  if ((flags & GETEVENTS) && minimum > r->cq_tail - load(&h->cq_head)) {
    struct thread *thread = cell_current_thread_internal();
    cell_retain_open_file(file);
    thread->wait =
      (struct thread_wait){.reason = WAIT_URING, .held_file = file, .target = minimum, .uring_submitted = done};
    return cell_sleep_poll(thread, frame, sigmask ? &mask : NULL, false, 0);
  }
  return done;
}

