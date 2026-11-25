#include "../usercopy.h"
#include "handlers.h"
#include "proc/accounting.h"
#include "proc/thread.h"
#include "ticks.h"

enum { EFAULT = 14, EINVAL = 22, TIMER_ABSTIME = 1 };

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
