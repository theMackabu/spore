#include "arch/aarch64/smp.h"

#include "arch/aarch64/exceptions.h"
#include "arch/aarch64/regs.h"
#include "arch/aarch64/timer.h"
#include "cell.h"
#include "kprintf.h"
#include <stddef.h>

extern void smp_secondary_entry(void);
extern void smp_enter_trap_frame(struct trap_frame *frame) __attribute__((noreturn));

volatile uint8_t smp_ap_parked[SPORE_MAX_CPUS];
volatile uint8_t smp_ap_el[SPORE_MAX_CPUS];
volatile uint8_t smp_ap_sctlr_m[SPORE_MAX_CPUS];
volatile uint8_t smp_ap_started[SPORE_MAX_CPUS];
uint8_t smp_secondary_stacks[SPORE_MAX_CPUS][64 * 1024] __attribute__((aligned(16)));
uint64_t smp_boot_ttbr_pa;
uint64_t smp_boot_tcr;
uint64_t smp_boot_mair;
uint64_t smp_boot_sctlr;
uint64_t smp_kernel_va_offset;
static uint32_t smp_parked_count = 1;
static volatile uint32_t smp_online_count = 1;
static volatile uint32_t smp_scheduler_started;
static volatile uint32_t smp_big_kernel_lock;

static int64_t psci_cpu_on(uint64_t target_mpidr, uint64_t entry_pa, uint64_t context_id) {
  register uint64_t x0 __asm__("x0") = 0xc4000003ull;
  register uint64_t x1 __asm__("x1") = target_mpidr;
  register uint64_t x2 __asm__("x2") = entry_pa;
  register uint64_t x3 __asm__("x3") = context_id;
  __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
  return (int64_t)x0;
}

static uint64_t virt_to_phys(uint64_t va, uint64_t kernel_phys_base, uint64_t kernel_virt_base) {
  return va - kernel_virt_base + kernel_phys_base;
}

void smp_boot_parked_secondaries(uint64_t kernel_phys_base, uint64_t kernel_virt_base) {
  smp_set_current_cpu(0);
  uint64_t ttbr1;
  __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
  __asm__ volatile("mrs %0, tcr_el1" : "=r"(smp_boot_tcr));
  __asm__ volatile("mrs %0, mair_el1" : "=r"(smp_boot_mair));
  __asm__ volatile("mrs %0, sctlr_el1" : "=r"(smp_boot_sctlr));
  smp_boot_ttbr_pa = ttbr1 & 0x0000fffffffff000ull;
  smp_kernel_va_offset = kernel_virt_base - kernel_phys_base;
  uint64_t entry_pa = virt_to_phys((uint64_t)(uintptr_t)smp_secondary_entry, kernel_phys_base, kernel_virt_base);
  smp_parked_count = 1;
  for (uint32_t cpu = 1; cpu < SPORE_MAX_CPUS; ++cpu) {
    smp_ap_parked[cpu] = 0;
    smp_ap_el[cpu] = 0;
    smp_ap_sctlr_m[cpu] = 0;
    smp_ap_started[cpu] = 0;
    int64_t rc = psci_cpu_on(cpu, entry_pa, cpu);
    if (rc != 0) {
      if (rc == -2) { break; }
      kprintf("[spore] smp: cpu%u cpu_on failed rc=%d\n", cpu, (int)rc);
      continue;
    }
    for (uint32_t spin = 0; spin < 1000000 && smp_ap_started[cpu] == 0; ++spin) {
      __asm__ volatile("dmb ish\n"
                       "yield\n"
                       :
                       :
                       : "memory");
    }
    if (smp_ap_started[cpu] != 0) {
      ++smp_parked_count;
    } else {
      kprintf("[spore] smp: cpu%u did not reach high-half entry stage=%u el=%u\n", cpu, (unsigned)smp_ap_parked[cpu],
              (unsigned)smp_ap_el[cpu]);
      kprintf("[spore] smp: cpu%u sctlr.m=%u\n", cpu, (unsigned)smp_ap_sctlr_m[cpu]);
    }
  }
  kprintf("[spore] smp: %u CPU%s booted\n", smp_parked_count, smp_parked_count == 1 ? "" : "s");
}

void smp_secondary_main(uint64_t cpu) {
  if (cpu < SPORE_MAX_CPUS) {
    smp_set_current_cpu((uint32_t)cpu);
    smp_ap_started[cpu] = 1;
    __asm__ volatile("dsb sy\nsev" : : : "memory");
  }
  while (smp_scheduler_started == 0) {
    __asm__ volatile("wfe");
  }
  exceptions_init();
  timer_cpu_init((uint32_t)cpu);
  __atomic_add_fetch(&smp_online_count, 1, __ATOMIC_RELAXED);
  struct trap_frame frame;
  for (;;) {
    smp_kernel_lock();
    cell_schedule(&frame);
    smp_kernel_unlock();
    smp_enter_trap_frame(&frame);
  }
}

void smp_start_scheduler_cpus(void) {
  smp_scheduler_started = 1;
  __asm__ volatile("dsb sy\nsev" : : : "memory");
}

uint32_t smp_parked_cpu_count(void) {
  return smp_parked_count;
}

uint32_t smp_online_cpu_count(void) {
  return smp_online_count;
}

uint32_t smp_possible_cpu_count(void) {
  return SPORE_MAX_CPUS;
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
