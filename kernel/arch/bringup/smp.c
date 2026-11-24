#include "arch/smp.h"
#include "arch/regs.h"
#include "arch/secondary.h"
#include "init/boot.h"
#include "limine.h"
#include "platform.h"
#include "proc/thread.h"
#include "user.h"

struct cpu_local {
  uintptr_t stack, scratch;
  uint64_t id;
};
static struct cpu_local cpus[SPORE_BOOT_CPU_MAX];
static uint64_t kernel_root;

uint32_t smp_current_cpu(void) {
  uint64_t id;
#ifdef __x86_64__
  __asm__ volatile("movq %%gs:16,%0" : "=r"(id));
#else
  __asm__ volatile("csrr %0,sscratch" : "=r"(id));
#endif
  return id;
}

void smp_set_current_cpu(uint32_t id) {
  cpus[id].id = id;
#ifdef __x86_64__
  uintptr_t base = (uintptr_t)&cpus[id];
  __asm__ volatile("wrmsr" ::"c"(0xc0000101u), "a"((uint32_t)base), "d"((uint32_t)(base >> 32)));
  __asm__ volatile("wrmsr" ::"c"(0xc0000102u), "a"(0), "d"(0));
#else
  __asm__ volatile("csrw sscratch,%0" ::"r"((uintptr_t)id));
#endif
}

void port_prepare_cpus(const struct spore_boot_info *boot) {
  kernel_root = active_root();
  smp_init_topology(boot->mp);
  for (uint32_t cpu = 0; cpu < smp_present_cpu_count(); ++cpu) {
    uintptr_t top = (uintptr_t)kernel_stack_top;
    if (cpu) {
      uint64_t pages = pmm_alloc_contiguous_pages(16);
      if (!pages) { boot_fail(BOOT_INIT_FAILED, "cannot allocate CPU stack"); }
      top = (uintptr_t)pmm_phys_to_virt(pages) + 16 * PAGE_SIZE;
    }
#ifdef __x86_64__
    extern bool x86_allocate_irq_stack(uint32_t cpu);
    if (!x86_allocate_irq_stack(cpu)) { boot_fail(BOOT_INIT_FAILED, "cannot allocate interrupt stack"); }
#endif
    cpus[cpu].stack = top - 16;
    *(uint64_t *)(top - 16) = cpu;
  }
  platform_cpu_init(0, cpus[0].stack);
  smp_boot_parked_secondaries();
}

void arch_prepare_secondary(uint32_t cpu) {
  install_root(kernel_root);
  platform_cpu_init(cpu, cpus[cpu].stack);
}
void arch_start_secondary_timer(uint32_t cpu) {
  (void)cpu;
}
void smp_secondary_main(const struct limine_mp_info *);
void smp_secondary_entry(struct limine_mp_info *info) {
  smp_secondary_main(info);
}
void smp_notify(void) {
  __atomic_thread_fence(__ATOMIC_RELEASE);
}

[[noreturn]] void smp_enter_trap_frame(struct trap_frame *f) {
  port_return_to_user(f, cpus[smp_current_cpu()].stack);
}

[[noreturn]] void user_enter(uint64_t pc, uint64_t sp) {
  (void)pc;
  (void)sp;
  smp_enter_trap_frame(&cell_current_thread_internal()->tf);
}

void port_idle_address_space(void) {
  if (kernel_root && active_root() != kernel_root) { install_root(kernel_root); }
}
