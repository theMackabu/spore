#include "user.h"

static constexpr uint64_t ADDRESS = 0x000ffffffffff000;
uint64_t active_root(void) {
  uint64_t cr3;
  __asm__ volatile("mov %%cr3,%0" : "=r"(cr3));
  return cr3 & ADDRESS;
}
void install_root(uint64_t pa) { __asm__ volatile("mov %0,%%cr3" :: "r"(pa) : "memory"); }
unsigned page_levels(void) { return 4; }
uint64_t entry_phys(uint64_t pte) { return pte & ADDRESS; }
uint64_t table_entry(uint64_t pa) { return pa | 7; }
uint64_t page_entry(uint64_t pa, unsigned flags) {
  return pa | 1 | ((flags & PAGE_WRITE) ? 2 : 0) | ((flags & PAGE_USER) ? 4 : 0) |
         ((flags & PAGE_EXEC) ? 0 : 1ull << 63);
}
bool page_allows(uint64_t pte, unsigned flags) {
  return (pte & 5) == 5 && (!(flags & PAGE_WRITE) || (pte & 2)) && (!(flags & PAGE_EXEC) || !(pte >> 63));
}

struct descriptor { uint16_t size; uint64_t base; } __attribute__((packed));
struct gate { uint16_t low, cs; uint8_t ist, type; uint16_t mid; uint32_t high, zero; };
struct task_state {
  uint32_t reserved; uint64_t rsp[3], reserved2, ist[7], reserved3;
  uint16_t reserved4, io_offset;
} __attribute__((packed));
static struct task_state tss;
static struct gate idt[256];
static uint64_t gdt[] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff, 0x00cff2000000ffff, 0x00affa000000ffff, 0, 0};
extern uint8_t kernel_stack_top[];
extern void syscall_entry(void), fault_entry(void), illegal_entry(void), divide_entry(void);
extern void enter_ring3(uint64_t pc, uint64_t sp, const struct descriptor *gdtr);
static void wrmsr(uint32_t msr, uint64_t value) {
  __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}
static void gate(unsigned nr, uintptr_t entry) {
  idt[nr] = (struct gate){entry, 8, 0, 0x8e, entry >> 16, entry >> 32, 0};
}

[[noreturn]] void user_enter(uint64_t pc, uint64_t sp) {
  uintptr_t t = (uintptr_t)&tss;
  tss.rsp[0] = (uintptr_t)kernel_stack_top;
  tss.io_offset = sizeof(tss);
  gdt[5] = (sizeof(tss) - 1) | ((t & 0xffffff) << 16) | (0x89ull << 40) | ((t & 0xff000000) << 32);
  gdt[6] = t >> 32;
  for (unsigned i = 0; i < 256; ++i) { gate(i, (uintptr_t)fault_entry); }
  gate(0, (uintptr_t)divide_entry);
  gate(6, (uintptr_t)illegal_entry);
  struct descriptor gdtr = {sizeof(gdt)-1, (uintptr_t)gdt}, idtr = {sizeof(idt)-1, (uintptr_t)idt};
  __asm__ volatile("cli; lidt %0" :: "m"(idtr));
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xc0000080u));
  wrmsr(0xc0000080, ((uint64_t)high << 32) | low | 0x801);
  wrmsr(0xc0000081, (0x13ull << 48) | (8ull << 32));
  wrmsr(0xc0000082, (uintptr_t)syscall_entry);
  wrmsr(0xc0000084, UINT32_MAX);
  enter_ring3(pc, sp, &gdtr);
  __builtin_unreachable();
}
