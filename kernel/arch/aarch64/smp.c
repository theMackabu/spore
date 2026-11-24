#include "arch/smp.h"
#include "arch/aarch64/exceptions.h"
#include "arch/aarch64/mmu.h"
#include "arch/aarch64/timer.h"
#include "arch/secondary.h"

void arch_prepare_secondary(uint32_t cpu) {
  (void)cpu;
  uint64_t mair;
  __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
  mair &= ~(0xffull << 16);
  __asm__ volatile("msr mair_el1, %0\nisb" : : "r"(mair) : "memory");
  aarch64_enable_ttbr0();
}

void arch_start_secondary_timer(uint32_t cpu) {
  exceptions_init();
  timer_cpu_init(cpu);
}

uint32_t smp_current_cpu(void) {
  uint64_t cpu;
  __asm__ volatile("mrs %0, tpidr_el1" : "=r"(cpu));
  return (uint32_t)cpu;
}

void smp_set_current_cpu(uint32_t cpu) {
  __asm__ volatile("msr tpidr_el1, %0" : : "r"((uint64_t)cpu) : "memory");
}

void smp_wait_for_irq(void) {
  __asm__ volatile("msr daifclr, #2\n"
                   "wfi\n"
                   "msr daifset, #2\n"
                   :
                   :
                   : "memory");
}

void smp_notify(void) {
  __asm__ volatile("sev" : : : "memory");
}
