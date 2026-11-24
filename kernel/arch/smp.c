#include "arch/smp.h"
#include "arch/regs.h"
#include "arch/secondary.h"
#include "lib/kprintf.h"
#include "limine.h"
#include "proc/thread.h"

#if defined(__aarch64__)
#define hardware_id(cpu) ((cpu)->mpidr)
#define bsp_id(mp) ((mp)->bsp_mpidr)
static inline void spin_relax(void) {
  __asm__ volatile("yield");
}
static inline void wait_event(void) {
  __asm__ volatile("wfe");
}
#elif defined(__x86_64__)
#define hardware_id(cpu) ((cpu)->lapic_id)
#define bsp_id(mp) ((mp)->bsp_lapic_id)
static inline void spin_relax(void) {
  __asm__ volatile("pause");
}
#define wait_event spin_relax
#else
#define hardware_id(cpu) ((cpu)->hartid)
#define bsp_id(mp) ((mp)->bsp_hartid)
static inline void spin_relax(void) {
  __asm__ volatile("nop");
}
#define wait_event spin_relax
#endif

struct smp_cpu {
  struct limine_mp_info *boot_cpu;
  void *current_thread;
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
  for (uint64_t i = 0; mp && i < mp->cpu_count; ++i) {
    struct limine_mp_info *cpu = mp->cpus[i];
    if (hardware_id(cpu) == bsp_id(mp)) {
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
      spin_relax();
    }
  }
  kprintf("[spore] smp: %u/%u CPU%s booted\n", started, smp_present_count, smp_present_count == 1 ? "" : "s");
}

[[noreturn]] void smp_secondary_main(const struct limine_mp_info *info) {
  uint32_t cpu = (uint32_t)info->extra_argument;
  smp_set_current_cpu(cpu);
  arch_prepare_secondary(cpu);
  __atomic_store_n(&smp_cpus[cpu].started, 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&smp_scheduler_started, __ATOMIC_ACQUIRE)) {
    wait_event();
  }
  arch_start_secondary_timer(cpu);
  smp_kernel_lock();
  if (cpu < smp_present_count) {
    __atomic_store_n(&smp_cpus[cpu].online, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&smp_online_count, 1, __ATOMIC_RELAXED);
  }
  smp_kernel_unlock();
  struct trap_frame frame;
  smp_kernel_lock();
  cell_schedule(&frame);
  smp_kernel_unlock();
  smp_enter_trap_frame(&frame);
}

void smp_start_scheduler_cpus(void) {
  __atomic_store_n(&smp_scheduler_started, 1, __ATOMIC_RELEASE);
#ifdef __aarch64__
  __asm__ volatile("dsb ishst" ::: "memory");
#endif
  smp_notify();
}

uint32_t smp_present_cpu_count(void) {
  return smp_present_count == 0 ? 1 : smp_present_count;
}

uint32_t smp_possible_cpu_count(void) {
  return smp_present_cpu_count();
}

uint32_t smp_online_cpu_count(void) {
  uint32_t online = __atomic_load_n(&smp_online_count, __ATOMIC_ACQUIRE);
  return online == 0 ? 1 : online;
}

bool smp_cpu_present(uint32_t cpu) {
  return cpu < smp_present_cpu_count();
}

bool smp_cpu_online(uint32_t cpu) {
  return cpu < smp_present_cpu_count() && __atomic_load_n(&smp_cpus[cpu].online, __ATOMIC_ACQUIRE) != 0;
}

uint64_t smp_cpu_hardware_id(uint32_t cpu) {
  return smp_cpu_present(cpu) && smp_cpus[cpu].boot_cpu ? hardware_id(smp_cpus[cpu].boot_cpu) : 0;
}

void smp_kernel_lock(void) {
  if (smp_present_count <= 1) { return; }
  while (__atomic_exchange_n(&smp_big_kernel_lock, 1, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(&smp_big_kernel_lock, __ATOMIC_RELAXED)) {
      wait_event();
    }
  }
}

void smp_kernel_unlock(void) {
  if (smp_present_count <= 1) { return; }
  __atomic_store_n(&smp_big_kernel_lock, 0, __ATOMIC_RELEASE);
  smp_notify();
}

void *smp_current_thread_slot(uint32_t cpu) {
  return cpu < smp_present_cpu_count() ? smp_cpus[cpu].current_thread : NULL;
}

void smp_set_current_thread_slot(uint32_t cpu, void *thread) {
  if (cpu < smp_present_cpu_count()) { smp_cpus[cpu].current_thread = thread; }
}

void smp_clear_current_thread_if(uint32_t cpu, void *thread) {
  if (cpu < smp_present_cpu_count() && smp_cpus[cpu].current_thread == thread) { smp_cpus[cpu].current_thread = NULL; }
}

bool smp_scheduler_waiting(uint32_t cpu) {
  return cpu < smp_present_cpu_count() && smp_cpus[cpu].scheduler_waiting != 0;
}

void smp_set_scheduler_waiting(uint32_t cpu, bool waiting) {
  if (cpu < smp_present_cpu_count()) { smp_cpus[cpu].scheduler_waiting = waiting ? 1u : 0u; }
}
