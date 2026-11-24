#include "abi/linux/shared/usercopy.h"
#include "abi/linux/shared/layout.h"
#include "handlers.h"
#include "abi/linux/shared/time/ticks.h"
#include "proc/domain.h"
#include "proc/poll.h"
#include "proc/thread.h"

#include <stdbool.h>
#include <stdint.h>

enum {
  EFAULT = 14,
  EINVAL = 22,
};

struct pselect6_sigmask64 {
  uint64_t sigmask;
  uint64_t sigsetsize;
};

static int read_wait_mask(uint64_t addr, uint64_t size, uint64_t *mask) {
  if (addr == 0) { return 0; }
  if (size != sizeof(*mask)) { return -EINVAL; }
  return syscall_user_readable(addr, sizeof(*mask)) && vmm_copy_from_user(syscall_active_as(), mask, addr, sizeof(*mask))
           ? 0 : -EFAULT;
}

static int read_timeout(uint64_t addr, uint64_t *ticks) {
  *ticks = 0;
  if (!addr) { return 0; }
  struct timespec64 timeout;
  if (!syscall_user_readable(addr, sizeof(timeout)) ||
      !vmm_copy_from_user(syscall_active_as(), &timeout, addr, sizeof(timeout))) { return -EFAULT; }
  if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000) { return -EINVAL; }
  if (timeout.tv_sec || timeout.tv_nsec) { *ticks = timespec_to_scheduler_ticks(&timeout); }
  return 0;
}

int64_t sys_ppoll(struct trap_frame *frame, uint64_t fds, uint64_t nfds, uint64_t timeout_addr, uint64_t sigmask,
                  uint64_t sigsetsize) {
  if (nfds > cell_current_domain_internal()->nofile_soft) { return -(int64_t)EINVAL; }
  uint64_t fds_bytes = nfds * sizeof(struct pollfd64);
  if ((nfds != 0 && fds_bytes / sizeof(struct pollfd64) != nfds) || !syscall_user_readable(fds, fds_bytes) ||
      !syscall_user_writable(fds, fds_bytes)) {
    return -(int64_t)EFAULT;
  }

  uint64_t timeout_ticks;
  int rc = read_timeout(timeout_addr, &timeout_ticks);
  if (rc) { return rc; }
  uint64_t mask;
  rc = read_wait_mask(sigmask, sigsetsize, &mask);
  if (rc < 0) { return rc; }
  return cell_ppoll_current(fds, nfds, timeout_addr != 0, timeout_ticks, sigmask ? &mask : NULL, frame);
}

int64_t sys_pselect6(struct trap_frame *frame, uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds,
                     uint64_t timeout_addr, uint64_t sigmask_data) {
  uint64_t timeout_ticks;
  int rc = read_timeout(timeout_addr, &timeout_ticks);
  if (rc) { return rc; }
  struct pselect6_sigmask64 signal_data = {0};
  if (sigmask_data != 0 &&
      (!syscall_user_readable(sigmask_data, sizeof(signal_data)) ||
       !vmm_copy_from_user(syscall_active_as(), &signal_data, sigmask_data, sizeof(signal_data)))) {
    return -(int64_t)EFAULT;
  }
  uint64_t mask;
  rc = read_wait_mask(signal_data.sigmask, signal_data.sigsetsize, &mask);
  if (rc < 0) { return rc; }
  return cell_pselect6_current(nfds, readfds, writefds, exceptfds, timeout_addr != 0, timeout_ticks,
                               signal_data.sigmask ? &mask : NULL, frame);
}

int64_t sys_epoll_pwait(struct trap_frame *frame, uint64_t epfd, uint64_t events_addr, uint64_t maxevents,
                        uint64_t timeout_ms, uint64_t sigmask, uint64_t sigsetsize) {
  uint64_t mask;
  int rc = read_wait_mask(sigmask, sigsetsize, &mask);
  if (rc < 0) { return rc; }
  return cell_epoll_wait_current((int)epfd, events_addr, (int)maxevents, (int)timeout_ms, sigmask ? &mask : NULL, frame);
}
