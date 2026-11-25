#include "proc/eventfd.h"

#include "proc/domain.h"
#include "proc/epoll.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "proc/poll.h"
#include "proc/thread.h"

enum { EAGAIN = 11, ENOMEM = 12, EFAULT = 14, EINVAL = 22, EMFILE = 24 };
enum { EFD_SEMAPHORE = 1, O_RDWR = 2, O_NONBLOCK = 04000, O_CLOEXEC = 02000000 };

int cell_fd_eventfd(uint32_t initial, int flags) {
  if ((flags & ~(O_CLOEXEC | O_NONBLOCK | EFD_SEMAPHORE)) != 0) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -ENOMEM; }
  struct open_file *file = cell_alloc_open_file();
  if (file == NULL) { return -ENOMEM; }
  file->type = OPEN_EVENTFD;
  file->flags = O_RDWR | (flags & O_NONBLOCK);
  file->eventfd_value = initial;
  file->eventfd_semaphore = (flags & EFD_SEMAPHORE) != 0;
  return cell_install_fd(domain, file, (flags & O_CLOEXEC));
}

static int transfer(struct domain *domain, struct open_file *file, bool write, uint64_t arg) {
  if (write) {
    if (UINT64_MAX - file->eventfd_value <= arg) { return -EAGAIN; }
    file->eventfd_value += arg;
  } else {
    if (file->eventfd_value == 0) { return -EAGAIN; }
    uint64_t value = file->eventfd_semaphore ? 1 : file->eventfd_value;
    if (!vmm_copy_to_user(cell_domain_as(domain), arg, &value, sizeof(value))) { return -EFAULT; }
    file->eventfd_value -= value;
  }
  return sizeof(uint64_t);
}

static void wake_waiters(struct open_file *file, unsigned activity) {
  bool progress;
  do {
    progress = false;
    size_t cursor = 0;
    struct thread *t;
    while (file->eventfd_waiters && (t = cell_next_waiter(WAIT_EVENTFD, &cursor))) {
      if (t->wait.held_file != file) { continue; }
      int rc = transfer(t->domain, file, t->wait.event_write, t->wait.event_arg);
      if (rc != -EAGAIN) {
        if (rc >= 0) { activity |= t->wait.event_write ? CELL_POLLIN : CELL_POLLOUT; }
        cell_finish_wait(t, rc);
        progress = true;
      }
    }
  } while (progress && file->eventfd_waiters);
  cell_epoll_file_activity(file, activity);
  cell_wake_poll_waiters_internal();
}

void cell_eventfd_signal(struct open_file *file) {
  if (file->eventfd_value != UINT64_MAX) { ++file->eventfd_value; }
  wake_waiters(file, CELL_POLLIN);
}

int64_t cell_eventfd_io(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len, bool write,
                        struct trap_frame *frame) {
  if (len < sizeof(uint64_t) || (write && len != sizeof(uint64_t))) { return -EINVAL; }
  uint64_t arg = buf;
  if (write) {
    if (!vmm_copy_from_user(cell_domain_as(domain), &arg, buf, sizeof(arg))) { return -EFAULT; }
    if (arg == UINT64_MAX) { return -EINVAL; }
  }
  int rc = transfer(domain, file, write, arg);
  if (rc >= 0) { wake_waiters(file, write ? CELL_POLLIN : CELL_POLLOUT); }
  if (rc != -EAGAIN || (file->flags & O_NONBLOCK) != 0 || frame == NULL) { return rc; }
  cell_retain_open_file(file);
  ++file->eventfd_waiters;
  return cell_block_current(
    frame, (struct thread_wait){.reason = WAIT_EVENTFD, .held_file = file, .event_write = write, .event_arg = arg});
}
