#include "user.h"
#include "platform.h"

static constexpr uint64_t ADDRESS = 0x000ffffffffff000;
uint64_t active_root(void) {
  uint64_t cr3;
  __asm__ volatile("mov %%cr3,%0" : "=r"(cr3));
  return cr3 & ADDRESS;
}
void install_root(uint64_t pa) {
  __asm__ volatile("mov %0,%%cr3" ::"r"(pa) : "memory");
}
unsigned page_levels(void) {
  return 4;
}
uint64_t entry_phys(uint64_t pte) {
  return pte & ADDRESS;
}
uint64_t table_entry(uint64_t pa) {
  return pa | 7;
}
uint64_t page_entry(uint64_t pa, unsigned flags) {
  return pa | 1 | ((flags & PAGE_DEVICE) ? 0x18 : 0) | ((flags & PAGE_WRITE) ? 2 : 0) | ((flags & PAGE_USER) ? 4 : 0) |
         ((flags & PAGE_EXEC) ? 0 : 1ull << 63);
}

struct descriptor {
  uint16_t size;
  uint64_t base;
} __attribute__((packed));
struct gate {
  uint16_t low, cs;
  uint8_t ist, type;
  uint16_t mid;
  uint32_t high, zero;
};
struct task_state {
  uint32_t reserved;
  uint64_t rsp[3], reserved2, ist[7], reserved3;
  uint16_t reserved4, io_offset;
} __attribute__((packed));
static struct task_state tasks[SPORE_BOOT_CPU_MAX];
static struct gate idt[256];
static const uint64_t segments[] = {
  0, 0x00af9a000000ffff, 0x00cf92000000ffff, 0x00cff2000000ffff, 0x00affa000000ffff, 0, 0};
static uint64_t tables[SPORE_BOOT_CPU_MAX][7];
extern void page_fault_entry(void), syscall_entry(void), fault_entry(void), illegal_entry(void), divide_entry(void);
extern void nmi_entry(void), spurious_entry(void), fault_error_entry(void);
static void wrmsr(uint32_t msr, uint64_t value) {
  __asm__ volatile("wrmsr" ::"c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}
static void gate(unsigned nr, uintptr_t entry) {
  idt[nr] = (struct gate){entry, 8, 0, 0x8e, entry >> 16, entry >> 32, 0};
}

bool x86_allocate_irq_stack(uint32_t cpu) {
  uint64_t page = pmm_alloc_page();
  if (!page) { return false; }
  tasks[cpu].ist[0] = (uintptr_t)pmm_phys_to_virt(page) + PAGE_SIZE;
  return true;
}

void platform_cpu_init(uint32_t cpu, uintptr_t stack) {
  uint64_t cr0, cr4;
  __asm__ volatile("mov %%cr0,%0; mov %%cr4,%1" : "=r"(cr0), "=r"(cr4));
  cr0 = (cr0 & ~12ull) | 2;
  cr4 = (cr4 & ~(1ull << 18)) | (3ull << 9);
  __asm__ volatile("mov %0,%%cr0; mov %1,%%cr4; fninit" ::"r"(cr0), "r"(cr4) : "memory");
  uint32_t mxcsr = 0x1f80;
  __asm__ volatile("ldmxcsr %0" ::"m"(mxcsr));
  struct task_state *tss = &tasks[cpu];
  uint64_t *gdt = tables[cpu];
  __builtin_memcpy(gdt, segments, sizeof(segments));
  uintptr_t t = (uintptr_t)tss;
  tss->rsp[0] = stack;
  tss->io_offset = sizeof(*tss);
  gdt[5] = (sizeof(*tss) - 1) | ((t & 0xffffff) << 16) | (0x89ull << 40) | ((t & 0xff000000) << 32);
  gdt[6] = t >> 32;
  if (!cpu) {
    for (unsigned i = 0; i < 256; ++i) {
      gate(i, (uintptr_t)fault_entry);
    }
    const unsigned with_error[] = {8, 10, 11, 12, 13, 17, 21, 29, 30};
    for (unsigned i = 0; i < sizeof(with_error) / sizeof(*with_error); ++i) {
      gate(with_error[i], (uintptr_t)fault_error_entry);
    }
    gate(0, (uintptr_t)divide_entry);
    gate(6, (uintptr_t)illegal_entry);
    gate(14, (uintptr_t)page_fault_entry);
    extern void timer_irq_entry(void), x86_timer_init(void);
    gate(32, (uintptr_t)timer_irq_entry);
    gate(2, (uintptr_t)nmi_entry);
    idt[2].ist = 1;
    gate(255, (uintptr_t)spurious_entry);
  }
  extern void x86_timer_init(void);
  x86_timer_init();
  struct descriptor gdtr = {sizeof(segments) - 1, (uintptr_t)gdt}, idtr = {sizeof(idt) - 1, (uintptr_t)idt};
  __asm__ volatile("cli; lidt %0" ::"m"(idtr));
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xc0000080u));
  wrmsr(0xc0000080, ((uint64_t)high << 32) | low | 0x801);
  wrmsr(0xc0000081, (0x13ull << 48) | (8ull << 32));
  wrmsr(0xc0000082, (uintptr_t)syscall_entry);
  wrmsr(0xc0000084, UINT32_MAX);
  __asm__ volatile("lgdt %0; pushq $8; leaq 1f(%%rip),%%rax; pushq %%rax; lretq; 1: mov $0x10,%%ax; mov %%ax,%%ss; mov "
                   "%%ax,%%ds; mov %%ax,%%es; mov $0x28,%%ax; ltr %%ax" ::"m"(gdtr)
                   : "ax", "memory");
}
