#include "arch/aarch64/smp.h"

#include "arch/aarch64/exceptions.h"
#include "arch/aarch64/regs.h"
#include "arch/aarch64/timer.h"
#include "cell.h"
#include "kprintf.h"
#include "limine.h"
#include "mm/vmm.h"
#include <stddef.h>

struct smp_cpu {
  struct limine_mp_info *boot_cpu;
  void *current_thread;
  uint64_t busy_ticks;
  uint64_t idle_ticks;
  volatile uint32_t started;
  volatile uint32_t online;
  volatile uint32_t scheduler_waiting;
};

extern void smp_secondary_entry(struct limine_mp_info *info);
extern void smp_enter_trap_frame(struct trap_frame *frame) __attribute__((noreturn));

uint32_t smp_present_count;

static struct smp_cpu smp_cpus[SPORE_BOOT_CPU_MAX];
static volatile uint32_t smp_online_count;
static volatile uint32_t smp_scheduler_started;
static volatile uint32_t smp_big_kernel_lock;

void smp_init_topology(const struct limine_mp_response *mp) {
  smp_set_current_cpu(0);
  smp_online_count = 1;
  smp_present_count = 1;
  smp_cpus[0].online = 1;
  for (uint64_t i = 0; i < mp->cpu_count; ++i) {
    struct limine_mp_info *cpu = mp->cpus[i];
    if (cpu->mpidr == mp->bsp_mpidr) {
      smp_cpus[0].boot_cpu = cpu;
    } else if (smp_present_count < SPORE_BOOT_CPU_MAX) {
      smp_cpus[smp_present_count++].boot_cpu = cpu;
    }
  }
  kprintf("[spore] smp: topology has %u present CPU%s\n", smp_present_count, smp_present_count == 1 ? "" : "s");
}

void smp_boot_parked_secondaries(void) {
  uint32_t started = 1;
  for (uint32_t cpu = 1; cpu < smp_present_count; ++cpu) {
    struct limine_mp_info *info = smp_cpus[cpu].boot_cpu;
    info->extra_argument = cpu;
    __atomic_store_n(&info->goto_address, smp_secondary_entry, __ATOMIC_RELEASE);
    for (uint32_t spin = 0; spin < 1000000; ++spin) {
      if (__atomic_load_n(&smp_cpus[cpu].started, __ATOMIC_ACQUIRE)) {
        ++started;
        break;
      }
      __asm__ volatile("yield");
    }
  }
  kprintf("[spore] smp: %u/%u CPU%s booted\n", started, smp_present_count, smp_present_count == 1 ? "" : "s");
}

void smp_secondary_main(const struct limine_mp_info *info) {
  uint32_t cpu = (uint32_t)info->extra_argument;
  smp_set_current_cpu(cpu);
  // Match the BSP's device-memory attribute and user translation regime.
  uint64_t mair;
  __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
  mair &= ~(0xffull << 16);
  __asm__ volatile("msr mair_el1, %0\nisb" : : "r"(mair) : "memory");
  vmm_enable_ttbr0();
  __atomic_store_n(&smp_cpus[cpu].started, 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&smp_scheduler_started, __ATOMIC_ACQUIRE)) {
    __asm__ volatile("wfe");
  }
  exceptions_init();
  timer_cpu_init((uint32_t)cpu);
  if (cpu < smp_present_count) {
    smp_cpus[cpu].online = 1;
    __atomic_add_fetch(&smp_online_count, 1, __ATOMIC_RELAXED);
  }
  struct trap_frame frame;
  for (;;) {
    smp_kernel_lock();
    cell_schedule(&frame);
    smp_kernel_unlock();
    smp_enter_trap_frame(&frame);
  }
}

void smp_start_scheduler_cpus(void) {
  __atomic_store_n(&smp_scheduler_started, 1, __ATOMIC_RELEASE);
  __asm__ volatile("dsb sy\nsev" : : : "memory");
}

uint32_t smp_present_cpu_count(void) {
  return smp_present_count == 0 ? 1 : smp_present_count;
}

uint32_t smp_possible_cpu_count(void) {
  return smp_present_cpu_count();
}

uint32_t smp_online_cpu_count(void) {
  uint32_t online = smp_online_count;
  return online == 0 ? 1 : online;
}

bool smp_cpu_present(uint32_t cpu) {
  return cpu < smp_present_cpu_count() && smp_cpus[cpu].boot_cpu != NULL;
}

bool smp_cpu_online(uint32_t cpu) {
  return cpu < smp_present_cpu_count() && smp_cpus[cpu].online != 0;
}

uint64_t smp_cpu_mpidr(uint32_t cpu) {
  return smp_cpu_present(cpu) ? smp_cpus[cpu].boot_cpu->mpidr : 0;
}

uint32_t smp_current_cpu(void) {
  uint64_t cpu;
  __asm__ volatile("mrs %0, tpidr_el1" : "=r"(cpu));
  return (uint32_t)cpu;
}

void smp_set_current_cpu(uint32_t cpu) {
  __asm__ volatile("msr tpidr_el1, %0" : : "r"((uint64_t)cpu) : "memory");
}

void smp_kernel_lock(void) {
  while (__atomic_exchange_n(&smp_big_kernel_lock, 1, __ATOMIC_ACQUIRE) != 0) {
    __asm__ volatile("wfe");
  }
}

void smp_kernel_unlock(void) {
  __atomic_store_n(&smp_big_kernel_lock, 0, __ATOMIC_RELEASE);
  __asm__ volatile("sev" : : : "memory");
}

void *smp_current_thread_slot(uint32_t cpu) {
  return cpu < smp_present_cpu_count() ? smp_cpus[cpu].current_thread : NULL;
}

void smp_set_current_thread_slot(uint32_t cpu, void *thread) {
  if (cpu < smp_present_cpu_count()) { smp_cpus[cpu].current_thread = thread; }
}

void smp_clear_current_thread_if(uint32_t cpu, void *thread) {
  if (cpu < smp_present_cpu_count() && smp_cpus[cpu].current_thread == thread) {
    smp_cpus[cpu].current_thread = NULL;
  }
}

bool smp_scheduler_waiting(uint32_t cpu) {
  return cpu < smp_present_cpu_count() && smp_cpus[cpu].scheduler_waiting != 0;
}

void smp_set_scheduler_waiting(uint32_t cpu, bool waiting) {
  if (cpu < smp_present_cpu_count()) { smp_cpus[cpu].scheduler_waiting = waiting ? 1u : 0u; }
}

void smp_note_cpu_busy_tick(uint32_t cpu) {
  if (cpu < smp_present_cpu_count()) { ++smp_cpus[cpu].busy_ticks; }
}

void smp_note_cpu_idle_tick(uint32_t cpu) {
  if (cpu < smp_present_cpu_count()) { ++smp_cpus[cpu].idle_ticks; }
}

uint64_t smp_cpu_busy_ticks(uint32_t cpu) {
  return cpu < smp_present_cpu_count() ? smp_cpus[cpu].busy_ticks : 0;
}

uint64_t smp_cpu_idle_ticks(uint32_t cpu) {
  return cpu < smp_present_cpu_count() ? smp_cpus[cpu].idle_ticks : 0;
}
