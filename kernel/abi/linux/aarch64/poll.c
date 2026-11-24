#include "abi/linux/aarch64/layout.h"
#include "arch/aarch64/regs.h"
#include "handlers.h"
#include "proc/accounting.h"
#include "proc/poll.h"
#include "proc/thread.h"

#include <stdbool.h>
#include <stdint.h>

enum {
  EFAULT = 14,
  EINVAL = 22,
  MAX_POLL_FDS = 64,
  TIMER_ABSTIME = 1,
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

static uint64_t timespec_to_scheduler_ticks(const struct timespec64 *timeout) {
  uint64_t ticks =
    (uint64_t)timeout->tv_sec * 100ull + ((uint64_t)timeout->tv_nsec * 100ull + 999999999ull) / 1000000000ull;
  return ticks == 0 ? 1 : ticks;
}

static uint64_t timespec_to_absolute_ticks(const struct timespec64 *timeout, uint64_t clock_id) {
  enum {
    CLOCK_REALTIME = 0,
    CLOCK_MONOTONIC = 1,
  };
  uint64_t base_sec = clock_id == CLOCK_REALTIME ? cell_boot_epoch_seconds() : 0;
  if ((uint64_t)timeout->tv_sec < base_sec) { return cell_uptime_ticks(); }
  return ((uint64_t)timeout->tv_sec - base_sec) * 100ull +
         ((uint64_t)timeout->tv_nsec * 100ull + 999999999ull) / 1000000000ull;
}

int64_t sys_nanosleep(struct trap_frame *frame, uint64_t req_addr, uint64_t rem_addr) {
  (void)rem_addr;
  struct timespec64 req;
  if (!syscall_user_readable(req_addr, sizeof(req)) ||
      !vmm_copy_from_user(syscall_active_as(), &req, req_addr, sizeof(req))) {
    return -(int64_t)EFAULT;
  }
  if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000) { return -(int64_t)EINVAL; }
  if (req.tv_sec == 0 && req.tv_nsec == 0) { return 0; }
  return cell_sleep_current(timespec_to_scheduler_ticks(&req), frame);
}

int64_t sys_clock_nanosleep(struct trap_frame *frame, uint64_t clock_id, uint64_t flags, uint64_t req_addr,
                            uint64_t rem_addr) {
  enum {
    CLOCK_REALTIME = 0,
    CLOCK_MONOTONIC = 1,
  };
  (void)rem_addr;
  if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) { return -(int64_t)EINVAL; }
  if ((flags & ~TIMER_ABSTIME) != 0) { return -(int64_t)EINVAL; }
  struct timespec64 req;
  if (!syscall_user_readable(req_addr, sizeof(req)) ||
      !vmm_copy_from_user(syscall_active_as(), &req, req_addr, sizeof(req))) {
    return -(int64_t)EFAULT;
  }
  if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000) { return -(int64_t)EINVAL; }
  if (req.tv_sec == 0 && req.tv_nsec == 0) { return 0; }
  if ((flags & TIMER_ABSTIME) != 0) {
    uint64_t deadline = timespec_to_absolute_ticks(&req, clock_id);
    if (deadline <= cell_uptime_ticks()) { return 0; }
    return cell_block_current_on_sleep(deadline, frame);
  }
  return cell_sleep_current(timespec_to_scheduler_ticks(&req), frame);
}

int64_t sys_ppoll(struct trap_frame *frame, uint64_t fds, uint64_t nfds, uint64_t timeout_addr, uint64_t sigmask,
                  uint64_t sigsetsize) {
  if (nfds > MAX_POLL_FDS) { return -(int64_t)EINVAL; }
  uint64_t fds_bytes = nfds * sizeof(struct pollfd64);
  if ((nfds != 0 && fds_bytes / sizeof(struct pollfd64) != nfds) || !syscall_user_readable(fds, fds_bytes) ||
      !syscall_user_writable(fds, fds_bytes)) {
    return -(int64_t)EFAULT;
  }

  struct timespec64 timeout = {.tv_sec = 0, .tv_nsec = 0};
  bool has_timeout = timeout_addr != 0;
  uint64_t timeout_ticks = 0;
  if (has_timeout) {
    if (!syscall_user_readable(timeout_addr, sizeof(timeout)) ||
        !vmm_copy_from_user(syscall_active_as(), &timeout, timeout_addr, sizeof(timeout))) {
      return -(int64_t)EFAULT;
    }
    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000) { return -(int64_t)EINVAL; }
    if (timeout.tv_sec != 0 || timeout.tv_nsec != 0) { timeout_ticks = timespec_to_scheduler_ticks(&timeout); }
  }
  uint64_t mask;
  int rc = read_wait_mask(sigmask, sigsetsize, &mask);
  if (rc < 0) { return rc; }
  return cell_ppoll_current(fds, nfds, has_timeout, timeout_ticks, sigmask ? &mask : NULL, frame);
}

int64_t sys_pselect6(struct trap_frame *frame, uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds,
                     uint64_t timeout_addr, uint64_t sigmask_data) {
  struct timespec64 timeout = {.tv_sec = 0, .tv_nsec = 0};
  bool has_timeout = timeout_addr != 0;
  uint64_t timeout_ticks = 0;
  if (has_timeout) {
    if (!syscall_user_readable(timeout_addr, sizeof(timeout)) ||
        !vmm_copy_from_user(syscall_active_as(), &timeout, timeout_addr, sizeof(timeout))) {
      return -(int64_t)EFAULT;
    }
    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000) { return -(int64_t)EINVAL; }
    if (timeout.tv_sec != 0 || timeout.tv_nsec != 0) { timeout_ticks = timespec_to_scheduler_ticks(&timeout); }
  }
  struct pselect6_sigmask64 signal_data = {0};
  if (sigmask_data != 0 &&
      (!syscall_user_readable(sigmask_data, sizeof(signal_data)) ||
       !vmm_copy_from_user(syscall_active_as(), &signal_data, sigmask_data, sizeof(signal_data)))) {
    return -(int64_t)EFAULT;
  }
  uint64_t mask;
  int rc = read_wait_mask(signal_data.sigmask, signal_data.sigsetsize, &mask);
  if (rc < 0) { return rc; }
  return cell_pselect6_current(nfds, readfds, writefds, exceptfds, has_timeout, timeout_ticks,
                               signal_data.sigmask ? &mask : NULL, frame);
}

int64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t event_addr) {
  uint32_t events = 0;
  uint64_t data = 0;
  if ((int)op != 2) {
    struct epoll_event64 event;
    if (!syscall_user_readable(event_addr, sizeof(event)) ||
        !vmm_copy_from_user(syscall_active_as(), &event, event_addr, sizeof(event))) {
      return -(int64_t)EFAULT;
    }
    events = event.events;
    data = event.data;
  }
  return cell_fd_epoll_ctl((int)epfd, (int)op, (int)fd, events, data);
}

int64_t sys_epoll_pwait(struct trap_frame *frame, uint64_t epfd, uint64_t events_addr, uint64_t maxevents,
                        uint64_t timeout_ms, uint64_t sigmask, uint64_t sigsetsize) {
  uint64_t mask;
  int rc = read_wait_mask(sigmask, sigsetsize, &mask);
  if (rc < 0) { return rc; }
  return cell_epoll_wait_current((int)epfd, events_addr, (int)maxevents, (int)timeout_ms, sigmask ? &mask : NULL, frame);
}
