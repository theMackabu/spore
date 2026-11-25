#include "proc/signalfd.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "proc/poll.h"
#include "proc/thread.h"
#include "arch/smp.h"
#include "abi/linux/shared/open.h"
#include "abi/linux/shared/signal.h"

struct signalfd_info {
  uint32_t signo;
  int32_t error, code;
  uint32_t pid, uid;
  int32_t fd;
  uint32_t tid, band, overrun, trapno;
  int32_t status;
  uint8_t rest[84];
};
static_assert(sizeof(struct signalfd_info) == 128);

constexpr unsigned SFD_NONBLOCK = 04000;

static size_t open_count;

int cell_signalfd(int fd, uint64_t mask, unsigned flags) {
  if (flags & ~(LINUX_O_CLOEXEC | SFD_NONBLOCK)) { return -22; }
  mask &= ~((1ull << (9 - 1)) | (1ull << (19 - 1)));
  if (fd == -1) {
    struct open_file *file = cell_alloc_open_file();
    if (!file) { return -12; }
    file->type = OPEN_SIGNALFD;
    file->flags = flags & SFD_NONBLOCK;
    file->signalfd_mask = mask;
    ++open_count;
    return cell_install_fd(cell_current_domain_internal(), file, flags & LINUX_O_CLOEXEC);
  }
  struct open_file *file = cell_fd_file(fd);
  if (!file) { return -9; }
  if (file->type != OPEN_SIGNALFD) { return -22; }
  file->signalfd_mask = mask;
  cell_signalfd_notify(NULL);
  return fd;
}

void cell_signalfd_close(void) { --open_count; }

int cell_signalfd_ready(const struct thread *thread, const struct open_file *file) {
  return thread && thread->domain &&
         ((thread->pending_signals | thread->domain->pending_signals) & file->signalfd_mask) != 0;
}

static int64_t read_pending(struct thread *t, struct open_file *file, uint64_t addr, uint64_t len) {
  uint64_t copied = 0;
  while (len - copied >= sizeof(struct signalfd_info)) {
    uint64_t available = (t->pending_signals | t->domain->pending_signals) & file->signalfd_mask;
    if (!available) { break; }
    int signo = __builtin_ctzll(available) + 1;
    struct siginfo_fields64 info = cell_pending_signal_info(t, signo, false);
    struct signalfd_info record = {.signo = signo, .code = info.si_code, .pid = info.sender.pid,
                                   .uid = info.sender.uid, .status = info.child_status};
    if (addr + copied < addr || !cell_domain_ensure_user_range(t->domain, addr + copied, sizeof(record), VMM_ACCESS_WRITE) ||
        !vmm_copy_to_user(cell_domain_as(t->domain), addr + copied, &record, sizeof(record))) {
      return copied ? (int64_t)copied : -14;
    }
    (void)cell_pending_signal_info(t, signo, true);
    copied += sizeof(record);
  }
  return copied ? (int64_t)copied : -11;
}

int64_t cell_signalfd_read(struct thread *thread, struct open_file *file, uint64_t addr, uint64_t len,
                           struct trap_frame *frame) {
  if (len < sizeof(struct signalfd_info)) { return -22; }
  int64_t rc = read_pending(thread, file, addr, len);
  if (rc >= 0) { cell_wake_poll_waiters_internal(); }
  if (rc != -11 || (file->flags & SFD_NONBLOCK) || !frame) { return rc; }
  cell_retain_open_file(file);
  return cell_block_current(frame, (struct thread_wait){.reason = WAIT_SIGNALFD, .held_file = file,
                                                        .buf = addr, .len = len});
}

void cell_signalfd_notify(struct domain *domain) {
  if (!open_count) { return; }
  size_t cursor = 0;
  struct thread *t;
  while ((t = cell_next_waiter(WAIT_SIGNALFD, &cursor))) {
    if (domain && domain != t->domain) { continue; }
    int64_t rc = read_pending(t, t->wait.held_file, t->wait.buf, t->wait.len);
    if (rc != -11) { cell_finish_wait(t, rc); smp_notify(); }
  }
  cell_wake_poll_waiters_internal();
}
