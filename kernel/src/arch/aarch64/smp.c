#include "arch/aarch64/smp.h"

#include "arch/aarch64/exceptions.h"
#include "arch/aarch64/regs.h"
#include "arch/aarch64/timer.h"
#include "cell.h"
#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"
#include <stddef.h>

enum {
  SMP_STACK_PAGES = 16,
};

struct smp_cpu {
  uint64_t mpidr;
  uint64_t stack_phys_top;
  uint64_t stack_virt_top;
  void *current_thread;
  uint64_t busy_ticks;
  uint64_t idle_ticks;
  volatile uint32_t started;
  volatile uint32_t online;
  volatile uint32_t scheduler_waiting;
  uint32_t flags;
};

struct smp_ap_boot_record {
  uint64_t logical_id;
  uint64_t stack_phys_top;
  uint64_t stack_virt_top;
  uint64_t ttbr_pa;
  uint64_t tcr;
  uint64_t mair;
  uint64_t sctlr;
  uint64_t kernel_va_offset;
};

extern void smp_secondary_entry(void);
extern void smp_enter_trap_frame(struct trap_frame *frame) __attribute__((noreturn));

uint32_t smp_present_count;

static struct smp_cpu *smp_cpus;
static struct smp_ap_boot_record *smp_boot_records;
static uint64_t smp_boot_records_pa;
static uint32_t smp_possible_count;
static volatile uint32_t smp_online_count;
static volatile uint32_t smp_scheduler_started;
static volatile uint32_t smp_big_kernel_lock;
static uint64_t smp_hhdm_offset;

static uint64_t current_mpidr(void) {
  uint64_t mpidr;
  __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return mpidr & 0xff00ffffffull;
}

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

static void *alloc_zero_page_backed_bytes(uint64_t bytes) {
  if (bytes == 0 || bytes > PAGE_SIZE) { return NULL; }
  uint64_t pa = pmm_alloc_zero_page();
  if (pa == 0) { return NULL; }
  return (void *)(uintptr_t)(smp_hhdm_offset + pa);
}

static void reorder_boot_cpu_first(void) {
  if (smp_cpus == NULL || smp_present_count == 0) { return; }
  uint64_t boot_mpidr = current_mpidr();
  for (uint32_t i = 0; i < smp_present_count; ++i) {
    if (smp_cpus[i].mpidr != boot_mpidr) { continue; }
    smp_cpus[i].flags |= SPORE_CPU_BOOT;
    if (i != 0) {
      struct smp_cpu tmp = smp_cpus[0];
      smp_cpus[0] = smp_cpus[i];
      smp_cpus[i] = tmp;
    }
    return;
  }
}

void smp_init_topology(uint64_t hhdm_offset, const struct spore_cpu_entry *entries, uint32_t count) {
  smp_hhdm_offset = hhdm_offset;
  smp_present_count = 0;
  smp_possible_count = 0;
  smp_online_count = 1;
  smp_scheduler_started = 0;
  smp_cpus = NULL;
  smp_boot_records = NULL;
  smp_boot_records_pa = 0;

  if (entries == NULL || count == 0) { count = 0; }
  if (count > SPORE_BOOT_CPU_MAX) { count = SPORE_BOOT_CPU_MAX; }
  if (count == 0) { count = 1; }

  uint64_t boot_records_pa = pmm_alloc_zero_page();
  smp_cpus = alloc_zero_page_backed_bytes(sizeof(struct smp_cpu) * count);
  smp_boot_records = boot_records_pa == 0 ? NULL : (struct smp_ap_boot_record *)(uintptr_t)(hhdm_offset + boot_records_pa);
  smp_boot_records_pa = boot_records_pa;
  if (smp_cpus == NULL || smp_boot_records == NULL || sizeof(struct smp_ap_boot_record) * count > PAGE_SIZE) {
    smp_present_count = 1;
    smp_possible_count = 1;
    return;
  }

  for (uint32_t i = 0; i < count; ++i) {
    uint64_t mpidr = entries == NULL ? current_mpidr() : entries[i].mpidr;
    smp_cpus[smp_present_count++] = (struct smp_cpu){
      .mpidr = mpidr & 0xff00ffffffull,
      .flags = entries == NULL ? (SPORE_CPU_PRESENT | SPORE_CPU_BOOT) : entries[i].flags,
    };
  }
  reorder_boot_cpu_first();
  smp_cpus[0].flags |= SPORE_CPU_PRESENT | SPORE_CPU_BOOT;
  smp_cpus[0].online = 1;

  for (uint32_t cpu = 1; cpu < smp_present_count; ++cpu) {
    uint64_t stack_pa = pmm_alloc_contiguous_pages(SMP_STACK_PAGES);
    if (stack_pa == 0) {
      kprintf("[spore] smp: cpu%u stack allocation failed, trimming topology\n", cpu);
      smp_present_count = cpu;
      break;
    }
    smp_cpus[cpu].stack_phys_top = stack_pa + SMP_STACK_PAGES * PAGE_SIZE;
    smp_cpus[cpu].stack_virt_top = hhdm_offset + stack_pa + SMP_STACK_PAGES * PAGE_SIZE;
    smp_boot_records[cpu].logical_id = cpu;
    smp_boot_records[cpu].stack_phys_top = smp_cpus[cpu].stack_phys_top;
    smp_boot_records[cpu].stack_virt_top = smp_cpus[cpu].stack_virt_top;
  }
  smp_possible_count = smp_present_count;
  kprintf("[spore] smp: topology has %u present CPU%s\n", smp_present_count,
          smp_present_count == 1 ? "" : "s");
}

void smp_boot_parked_secondaries(uint64_t kernel_phys_base, uint64_t kernel_virt_base) {
  smp_set_current_cpu(0);
  uint64_t ttbr1;
  uint64_t boot_tcr;
  uint64_t boot_mair;
  uint64_t boot_sctlr;
  __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
  __asm__ volatile("mrs %0, tcr_el1" : "=r"(boot_tcr));
  __asm__ volatile("mrs %0, mair_el1" : "=r"(boot_mair));
  __asm__ volatile("mrs %0, sctlr_el1" : "=r"(boot_sctlr));
  uint64_t boot_ttbr_pa = ttbr1 & 0x0000fffffffff000ull;
  uint64_t kernel_va_offset = kernel_virt_base - kernel_phys_base;
  uint64_t entry_pa = virt_to_phys((uint64_t)(uintptr_t)smp_secondary_entry, kernel_phys_base, kernel_virt_base);
  uint32_t started = 1;
  for (uint32_t cpu = 1; cpu < smp_present_count; ++cpu) {
    smp_cpus[cpu].started = 0;
    smp_boot_records[cpu].ttbr_pa = boot_ttbr_pa;
    smp_boot_records[cpu].tcr = boot_tcr;
    smp_boot_records[cpu].mair = boot_mair;
    smp_boot_records[cpu].sctlr = boot_sctlr;
    smp_boot_records[cpu].kernel_va_offset = kernel_va_offset;
    uint64_t context_pa = smp_boot_records_pa + sizeof(struct smp_ap_boot_record) * cpu;
    int64_t rc = psci_cpu_on(smp_cpus[cpu].mpidr, entry_pa, context_pa);
    if (rc != 0) {
      kprintf("[spore] smp: cpu%u mpidr=0x%x cpu_on failed rc=%d\n", cpu, (unsigned)smp_cpus[cpu].mpidr, (int)rc);
      continue;
    }
    for (uint32_t spin = 0; spin < 1000000 && smp_cpus[cpu].started == 0; ++spin) {
      __asm__ volatile("dmb ish\n"
                       "yield\n"
                       :
                       :
                       : "memory");
    }
    if (smp_cpus[cpu].started != 0) {
      ++started;
    } else {
      kprintf("[spore] smp: cpu%u mpidr=0x%x did not reach high-half entry\n", cpu, (unsigned)smp_cpus[cpu].mpidr);
    }
  }
  kprintf("[spore] smp: %u/%u CPU%s booted\n", started, smp_present_count, smp_present_count == 1 ? "" : "s");
}

void smp_secondary_main(uint64_t cpu) {
  if (cpu < smp_present_count) {
    smp_set_current_cpu((uint32_t)cpu);
    smp_cpus[cpu].started = 1;
    __asm__ volatile("dsb sy\nsev" : : : "memory");
  }
  while (smp_scheduler_started == 0) {
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
  smp_scheduler_started = 1;
  __asm__ volatile("dsb sy\nsev" : : : "memory");
}

uint32_t smp_present_cpu_count(void) {
  return smp_present_count == 0 ? 1 : smp_present_count;
}

uint32_t smp_possible_cpu_count(void) {
  return smp_possible_count == 0 ? smp_present_cpu_count() : smp_possible_count;
}

uint32_t smp_online_cpu_count(void) {
  uint32_t online = smp_online_count;
  return online == 0 ? 1 : online;
}

bool smp_cpu_present(uint32_t cpu) {
  return cpu < smp_present_cpu_count() && smp_cpus != NULL && (smp_cpus[cpu].flags & SPORE_CPU_PRESENT) != 0;
}

bool smp_cpu_online(uint32_t cpu) {
  return cpu < smp_present_cpu_count() && smp_cpus != NULL && smp_cpus[cpu].online != 0;
}

uint64_t smp_cpu_mpidr(uint32_t cpu) {
  return cpu < smp_present_cpu_count() && smp_cpus != NULL ? smp_cpus[cpu].mpidr : 0;
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
  return cpu < smp_present_cpu_count() && smp_cpus != NULL ? smp_cpus[cpu].current_thread : NULL;
}

void smp_set_current_thread_slot(uint32_t cpu, void *thread) {
  if (cpu < smp_present_cpu_count() && smp_cpus != NULL) { smp_cpus[cpu].current_thread = thread; }
}

void smp_clear_current_thread_if(uint32_t cpu, void *thread) {
  if (cpu < smp_present_cpu_count() && smp_cpus != NULL && smp_cpus[cpu].current_thread == thread) {
    smp_cpus[cpu].current_thread = NULL;
  }
}

bool smp_scheduler_waiting(uint32_t cpu) {
  return cpu < smp_present_cpu_count() && smp_cpus != NULL && smp_cpus[cpu].scheduler_waiting != 0;
}

void smp_set_scheduler_waiting(uint32_t cpu, bool waiting) {
  if (cpu < smp_present_cpu_count() && smp_cpus != NULL) { smp_cpus[cpu].scheduler_waiting = waiting ? 1u : 0u; }
}

void smp_note_cpu_busy_tick(uint32_t cpu) {
  if (cpu < smp_present_cpu_count() && smp_cpus != NULL) { ++smp_cpus[cpu].busy_ticks; }
}

void smp_note_cpu_idle_tick(uint32_t cpu) {
  if (cpu < smp_present_cpu_count() && smp_cpus != NULL) { ++smp_cpus[cpu].idle_ticks; }
}

uint64_t smp_cpu_busy_ticks(uint32_t cpu) {
  return cpu < smp_present_cpu_count() && smp_cpus != NULL ? smp_cpus[cpu].busy_ticks : 0;
}

uint64_t smp_cpu_idle_ticks(uint32_t cpu) {
  return cpu < smp_present_cpu_count() && smp_cpus != NULL ? smp_cpus[cpu].idle_ticks : 0;
}
