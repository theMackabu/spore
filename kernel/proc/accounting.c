#include "proc/accounting.h"
#include "accounting_internal.h"
#include "arch/aarch64/smp.h"
#include "proc/domain.h"
#include "proc/thread.h"

static struct {
  uint64_t busy_ticks;
  uint64_t idle_ticks;
} cpu_usage[SPORE_BOOT_CPU_MAX];

static uint64_t scheduler_ticks;
static uint64_t boot_epoch_sec;
static uint64_t loadavg_scaled[3];
static uint64_t next_loadavg_tick;

enum {
  LOADAVG_FSHIFT = 11,
  LOADAVG_FSCALE = 1u << LOADAVG_FSHIFT,
  LOADAVG_INTERVAL_TICKS = 5 * 100,
  LOADAVG_EXP_1 = 1884,
  LOADAVG_EXP_5 = 2014,
  LOADAVG_EXP_15 = 2037,
};

static uint64_t runnable_thread_count(void) {
  uint64_t count = 0;
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain != NULL && thread->state == THREAD_RUNNABLE) { ++count; }
  }
  return count;
}

static uint64_t loadavg_step(uint64_t old, uint64_t active, uint64_t exp) {
  return (old * exp + active * LOADAVG_FSCALE * (LOADAVG_FSCALE - exp)) >> LOADAVG_FSHIFT;
}

static void update_loadavg(void) {
  if (next_loadavg_tick == 0) { next_loadavg_tick = LOADAVG_INTERVAL_TICKS; }
  while (scheduler_ticks >= next_loadavg_tick) {
    uint64_t active = runnable_thread_count();
    loadavg_scaled[0] = loadavg_step(loadavg_scaled[0], active, LOADAVG_EXP_1);
    loadavg_scaled[1] = loadavg_step(loadavg_scaled[1], active, LOADAVG_EXP_5);
    loadavg_scaled[2] = loadavg_step(loadavg_scaled[2], active, LOADAVG_EXP_15);
    next_loadavg_tick += LOADAVG_INTERVAL_TICKS;
  }
}

void cell_accounting_reset(void) {
  scheduler_ticks = 0;
  loadavg_scaled[0] = 0;
  loadavg_scaled[1] = 0;
  loadavg_scaled[2] = 0;
  next_loadavg_tick = LOADAVG_INTERVAL_TICKS;
}

uint64_t cell_account_tick(uint32_t cpu, bool from_lower_el, bool idle) {
  if (cpu < smp_present_cpu_count()) {
    if (from_lower_el) {
      ++cpu_usage[cpu].busy_ticks;
    } else if (idle) {
      ++cpu_usage[cpu].idle_ticks;
    }
  }
  if (cpu != 0) { return scheduler_ticks; }
  ++scheduler_ticks;
  update_loadavg();
  return scheduler_ticks;
}

bool cell_charge_cpu_budget(struct domain *domain, bool from_lower_el) {
  if (from_lower_el) { ++domain->cpu_ticks; }
  if (!domain->budget.max_ticks || !domain->budget.remaining_ticks) { return false; }
  return --domain->budget.remaining_ticks == 0;
}

void cell_set_boot_epoch(uint64_t epoch_sec) {
  boot_epoch_sec = epoch_sec;
}

uint64_t cell_realtime_seconds(void) {
  return boot_epoch_sec + scheduler_ticks / 100;
}

uint64_t cell_uptime_ticks(void) {
  return scheduler_ticks;
}

uint64_t cell_cpu_busy_ticks(uint32_t cpu) {
  return cpu < smp_present_cpu_count() ? cpu_usage[cpu].busy_ticks : 0;
}

uint64_t cell_cpu_idle_ticks(uint32_t cpu) {
  return cpu < smp_present_cpu_count() ? cpu_usage[cpu].idle_ticks : 0;
}

void cell_loadavg_scaled(uint64_t out[3]) {
  if (out == NULL) { return; }
  out[0] = loadavg_scaled[0];
  out[1] = loadavg_scaled[1];
  out[2] = loadavg_scaled[2];
}

uint64_t cell_boot_epoch_seconds(void) {
  return boot_epoch_sec;
}
