#include "arch/aarch64/syscall_handlers.h"

#include "cell.h"
#include "mem.h"
#include "mm/pmm.h"
#include "random.h"

#include <stddef.h>
#include <stdint.h>

enum {
  EFAULT = 14,
  EINVAL = 22,
  CLOCK_REALTIME = 0,
  CLOCK_MONOTONIC = 1,
};

struct timespec64 {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct timeval64 {
  int64_t tv_sec;
  int64_t tv_usec;
};

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

struct rusage64 {
  struct timeval64 ru_utime;
  struct timeval64 ru_stime;
  int64_t ru_maxrss;
  int64_t ru_ixrss;
  int64_t ru_idrss;
  int64_t ru_isrss;
  int64_t ru_minflt;
  int64_t ru_majflt;
  int64_t ru_nswap;
  int64_t ru_inblock;
  int64_t ru_oublock;
  int64_t ru_msgsnd;
  int64_t ru_msgrcv;
  int64_t ru_nsignals;
  int64_t ru_nvcsw;
  int64_t ru_nivcsw;
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

int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp) {
  uint64_t cnt;
  uint64_t freq;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  uint64_t elapsed_cnt = cnt;
  uint64_t base_sec = 0;
  if (clk_id == CLOCK_REALTIME) {
    uint64_t base_cnt;
    uint64_t base_freq;
    syscall_realtime_base(&base_sec, &base_cnt, &base_freq);
    if (cnt >= base_cnt) { elapsed_cnt = cnt - base_cnt; }
    if (base_freq != 0) { freq = base_freq; }
  } else if (clk_id != CLOCK_MONOTONIC) {
    return -(int64_t)EINVAL;
  }
  struct timespec64 ts = {
    .tv_sec = (int64_t)(base_sec + elapsed_cnt / freq),
    .tv_nsec = (int64_t)(((elapsed_cnt % freq) * 1000000000ull) / freq),
  };
  return syscall_user_writable(tp, sizeof(ts)) && vmm_copy_to_user(syscall_active_as(), tp, &ts, sizeof(ts))
           ? 0
           : -(int64_t)EFAULT;
}

int64_t sys_clock_getres(uint64_t clk_id, uint64_t tp) {
  if (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC) { return -(int64_t)EINVAL; }
  if (tp == 0) { return 0; }
  uint64_t freq;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  int64_t nsec = 10000000;
  if (freq != 0) {
    uint64_t rounded = (1000000000ull + freq - 1) / freq;
    nsec = (int64_t)(rounded == 0 ? 1 : rounded);
  }
  struct timespec64 ts = {.tv_sec = 0, .tv_nsec = nsec};
  return syscall_user_writable(tp, sizeof(ts)) && vmm_copy_to_user(syscall_active_as(), tp, &ts, sizeof(ts))
           ? 0
           : -(int64_t)EFAULT;
}

int64_t sys_gettimeofday(uint64_t tv_addr, uint64_t tz_addr) {
  uint64_t cnt;
  uint64_t freq;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  uint64_t base_sec;
  uint64_t base_cnt;
  uint64_t base_freq;
  syscall_realtime_base(&base_sec, &base_cnt, &base_freq);
  uint64_t elapsed_cnt = cnt >= base_cnt ? cnt - base_cnt : cnt;
  if (base_freq != 0) { freq = base_freq; }
  if (tv_addr != 0) {
    struct timeval64 tv = {
      .tv_sec = (int64_t)(base_sec + elapsed_cnt / freq),
      .tv_usec = (int64_t)(((elapsed_cnt % freq) * 1000000ull) / freq),
    };
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
  uint64_t cnt;
  uint64_t freq;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  int64_t ticks = (int64_t)((cnt * 100ull) / freq);
  if (buf_addr != 0) {
    struct tms64 tms = {.tms_utime = ticks, .tms_stime = 0, .tms_cutime = 0, .tms_cstime = 0};
    if (!syscall_user_writable(buf_addr, sizeof(tms)) ||
        !vmm_copy_to_user(syscall_active_as(), buf_addr, &tms, sizeof(tms))) {
      return -(int64_t)EFAULT;
    }
  }
  return ticks;
}

static void ticks_to_timeval(uint64_t ticks, struct timeval64 *tv) {
  tv->tv_sec = (int64_t)(ticks / 100);
  tv->tv_usec = (int64_t)((ticks % 100) * 10000);
}

int64_t sys_getrusage(int who, uint64_t usage_addr) {
  enum {
    RUSAGE_SELF = 0,
    RUSAGE_CHILDREN = -1,
    RUSAGE_THREAD = 1,
  };
  if (usage_addr == 0 || !syscall_user_writable(usage_addr, sizeof(struct rusage64))) {
    return -(int64_t)EFAULT;
  }
  if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) { return -(int64_t)EINVAL; }

  struct rusage64 usage;
  kmemset(&usage, 0, sizeof(usage));
  if (who == RUSAGE_SELF || who == RUSAGE_THREAD) {
    struct proc_info procs[MAX_DOMAINS];
    size_t count = cell_proc_info(procs, MAX_DOMAINS);
    int pid = cell_current_pid();
    for (size_t i = 0; i < count && i < MAX_DOMAINS; ++i) {
      if ((int)procs[i].pid != pid) { continue; }
      ticks_to_timeval(procs[i].cpu_ticks, &usage.ru_utime);
      usage.ru_maxrss = (int64_t)((procs[i].resident_pages * PAGE_SIZE) / 1024);
      usage.ru_minflt = (int64_t)procs[i].minor_faults;
      usage.ru_majflt = (int64_t)procs[i].major_faults;
      break;
    }
  }
  return vmm_copy_to_user(syscall_active_as(), usage_addr, &usage, sizeof(usage)) ? 0 : -(int64_t)EFAULT;
}
