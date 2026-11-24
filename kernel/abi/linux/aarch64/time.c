#include "abi/linux/aarch64/layout.h"
#include "arch/aarch64/timer.h"
#include "handlers.h"
#include "proc/accounting.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/thread.h"
#include "lib/random.h"

#include <stddef.h>
#include <stdint.h>

enum {
  EFAULT = 14,
  EINVAL = 22,
  CLOCK_REALTIME = 0,
  CLOCK_MONOTONIC = 1,
  CLOCK_PROCESS_CPUTIME_ID = 2,
  CLOCK_THREAD_CPUTIME_ID = 3,
};

struct itimerval64 {
  struct timeval64 interval;
  struct timeval64 value;
};

static bool timer_ticks(struct timeval64 tv, uint64_t *ticks) {
  if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) { return false; }
  uint64_t fraction = ((uint64_t)tv.tv_usec + 9999) / 10000;
  if ((uint64_t)tv.tv_sec > (UINT64_MAX - fraction) / 100) { return false; }
  *ticks = (uint64_t)tv.tv_sec * 100 + fraction;
  return true;
}

static struct timeval64 ticks_to_timeval(uint64_t ticks) {
  return (struct timeval64){.tv_sec = ticks / 100, .tv_usec = (ticks % 100) * 10000};
}

int64_t sys_setitimer(uint64_t which, uint64_t new_addr, uint64_t old_addr) {
  if (which != 0) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -EINVAL; }
  struct itimerval64 next = {0};
  uint64_t delay, interval, now = cell_uptime_ticks();
  if (new_addr && (!syscall_user_readable(new_addr, sizeof(next)) ||
                   !vmm_copy_from_user(syscall_active_as(), &next, new_addr, sizeof(next)))) {
    return -EFAULT;
  }
  if (!timer_ticks(next.value, &delay) || !timer_ticks(next.interval, &interval) || delay > UINT64_MAX - now) {
    return -EINVAL;
  }
  if (old_addr) {
    uint64_t remaining = domain->alarm_deadline > now ? domain->alarm_deadline - now : 0;
    struct itimerval64 old = {.value = ticks_to_timeval(remaining),
                              .interval = ticks_to_timeval(domain->alarm_interval)};
    if (!syscall_user_writable(old_addr, sizeof(old)) ||
        !vmm_copy_to_user(syscall_active_as(), old_addr, &old, sizeof(old))) {
      return -EFAULT;
    }
  }
  domain->alarm_deadline = delay ? now + delay : 0;
  domain->alarm_interval = delay ? interval : 0;
  return 0;
}

struct timezone32 {
  int32_t tz_minuteswest;
  int32_t tz_dsttime;
};

struct tms64 {
  int64_t tms_utime;
  int64_t tms_stime;
  int64_t tms_cutime;
  int64_t tms_cstime;
};

int64_t sys_getrandom(uint64_t buf, uint64_t len) {
  if (!syscall_user_writable(buf, len)) { return -(int64_t)EFAULT; }
  uint8_t tmp[128];
  uint64_t done = 0;
  while (done < len) {
    uint64_t chunk = len - done;
    if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
    random_bytes(tmp, (size_t)chunk);
    if (!vmm_copy_to_user(syscall_active_as(), buf + done, tmp, (size_t)chunk)) {
      kmemset(tmp, 0, sizeof(tmp));
      return -(int64_t)EFAULT;
    }
    done += chunk;
  }
  kmemset(tmp, 0, sizeof(tmp));
  return (int64_t)len;
}

static bool cpu_clock(uint64_t id) {
  return id == CLOCK_PROCESS_CPUTIME_ID || id == CLOCK_THREAD_CPUTIME_ID;
}

static int write_timespec(uint64_t addr, struct timespec64 ts) {
  return syscall_user_writable(addr, sizeof(ts)) && vmm_copy_to_user(syscall_active_as(), addr, &ts, sizeof(ts))
           ? 0 : -EFAULT;
}

static struct timespec64 wall_time(bool realtime) {
  uint64_t cnt = aarch64_counter_value(), freq = aarch64_counter_frequency(), epoch = 0;
  if (realtime) {
    uint64_t base, base_freq;
    syscall_realtime_base(&epoch, &base, &base_freq);
    if (cnt >= base) { cnt -= base; }
    if (base_freq) { freq = base_freq; }
  }
  return (struct timespec64){.tv_sec = epoch + cnt / freq, .tv_nsec = (cnt % freq) * 1000000000ULL / freq};
}

int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp) {
  if (cpu_clock(clk_id)) {
    const struct thread *thread = cell_current_thread_internal();
    uint64_t ticks = clk_id == CLOCK_THREAD_CPUTIME_ID ? thread->cpu_ticks : thread->domain->cpu_ticks;
    return write_timespec(tp, (struct timespec64){.tv_sec = ticks / 100, .tv_nsec = (ticks % 100) * 10000000});
  }
  if (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC) { return -EINVAL; }
  return write_timespec(tp, wall_time(clk_id == CLOCK_REALTIME));
}

int64_t sys_clock_getres(uint64_t clk_id, uint64_t tp) {
  uint64_t freq;
  if (cpu_clock(clk_id)) { freq = 100; }
  else if (clk_id == CLOCK_REALTIME || clk_id == CLOCK_MONOTONIC) { freq = aarch64_counter_frequency(); }
  else { return -EINVAL; }
  if (!tp) { return 0; }
  return write_timespec(tp, (struct timespec64){.tv_nsec = (1000000000ULL + freq - 1) / freq});
}

int64_t sys_gettimeofday(uint64_t tv_addr, uint64_t tz_addr) {
  if (tv_addr != 0) {
    struct timespec64 ts = wall_time(true);
    struct timeval64 tv = {.tv_sec = ts.tv_sec, .tv_usec = ts.tv_nsec / 1000};
    if (!syscall_user_writable(tv_addr, sizeof(tv)) ||
        !vmm_copy_to_user(syscall_active_as(), tv_addr, &tv, sizeof(tv))) {
      return -(int64_t)EFAULT;
    }
  }
  if (tz_addr != 0) {
    struct timezone32 tz = {.tz_minuteswest = 0, .tz_dsttime = 0};
    if (!syscall_user_writable(tz_addr, sizeof(tz)) ||
        !vmm_copy_to_user(syscall_active_as(), tz_addr, &tz, sizeof(tz))) {
      return -(int64_t)EFAULT;
    }
  }
  return 0;
}

int64_t sys_times(uint64_t buf_addr) {
  if (buf_addr != 0) {
    const struct domain *domain = cell_current_domain_internal();
    struct tms64 tms = {.tms_utime = domain->cpu_ticks};
    if (!syscall_user_writable(buf_addr, sizeof(tms)) ||
        !vmm_copy_to_user(syscall_active_as(), buf_addr, &tms, sizeof(tms))) {
      return -(int64_t)EFAULT;
    }
  }
  struct timespec64 ts = wall_time(false);
  return ts.tv_sec * 100 + ts.tv_nsec / 10000000;
}

int64_t sys_getrusage(int who, uint64_t usage_addr) {
  enum {
    RUSAGE_SELF = 0,
    RUSAGE_CHILDREN = -1,
    RUSAGE_THREAD = 1,
  };
  if (usage_addr == 0 || !syscall_user_writable(usage_addr, sizeof(struct rusage64))) { return -(int64_t)EFAULT; }
  if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) { return -(int64_t)EINVAL; }

  struct rusage64 usage;
  kmemset(&usage, 0, sizeof(usage));
  if (who == RUSAGE_SELF || who == RUSAGE_THREAD) {
    const struct thread *thread = cell_current_thread_internal();
    if (thread != NULL && thread->domain != NULL) {
      const struct domain *d = thread->domain;
      const struct user_address_space *as = cell_domain_as_const(d);
      uint64_t peak = as && as->peak_resident_pages > d->previous_peak_rss ? as->peak_resident_pages : d->previous_peak_rss;
      usage.ru_utime = ticks_to_timeval(who == RUSAGE_THREAD ? thread->cpu_ticks : d->cpu_ticks);
      usage.ru_maxrss = (int64_t)(peak * (PAGE_SIZE / 1024));
      usage.ru_minflt = (int64_t)d->minor_faults;
      usage.ru_majflt = (int64_t)d->major_faults;
    }
  }
  return vmm_copy_to_user(syscall_active_as(), usage_addr, &usage, sizeof(usage)) ? 0 : -(int64_t)EFAULT;
}
