#include "platform.h"
#include "arch/smp.h"
#include "mm/pmm.h"

static long sbi(unsigned extension, unsigned function, unsigned long first, unsigned long second) {
  register unsigned long a0 __asm__("a0") = first;
  register unsigned long a1 __asm__("a1") = second;
  register unsigned long a2 __asm__("a2") = 0;
  register unsigned long a6 __asm__("a6") = function;
  register unsigned long a7 __asm__("a7") = extension;
  __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(a6), "r"(a7) : "memory");
  return a0 == 0 ? (long)a1 : -1;
}
constexpr unsigned DBCN = 0x4442434e;
static uint64_t input_page;
void console_init(void) {}
void console_putc(char c) {
  sbi(DBCN, 2, (unsigned char)c, 0);
}
int console_try_getc(void) {
  if (!input_page) { input_page = pmm_alloc_page(); }
  if (!input_page) { return -1; }
  long n = sbi(DBCN, 1, 1, input_page);
  return n == 1 ? *(uint8_t *)pmm_phys_to_virt(input_page) : -1;
}
[[noreturn]] void platform_poweroff(void) {
  sbi(0x53525354, 0, 0, 0);
  for (;;) {
    __asm__ volatile("wfi");
  }
}

int console_getc(void) {
  int value;
  do {
    value = console_try_getc();
  } while (value < 0);
  return value;
}
[[noreturn]] void arch_reboot(void) {
  sbi(0x53525354, 0, 1, 0);
  for (;;) {
    __asm__ volatile("wfi");
  }
}

void platform_rearm_timer(void) {
  uint64_t now;
  __asm__ volatile("rdtime %0" : "=r"(now));
  sbi(0x54494d45, 0, now + 100000, 0);
}
void platform_wait_tick(void) {
  __asm__ volatile("wfi");
  platform_rearm_timer();
}

static void remote_fence(long function) {
  if (smp_online_cpu_count() <= 1) { return; }
  register long a0 __asm__("a0") = 0, a1 __asm__("a1") = -1;
  register long a2 __asm__("a2") = 0, a3 __asm__("a3") = -1;
  register long a6 __asm__("a6") = function, a7 __asm__("a7") = 0x52464e43;
  __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(a3), "r"(a6), "r"(a7) : "memory");
  if (a0) { platform_poweroff(); }
}

void port_flush_remote(void) {
  remote_fence(1);
}

int riscv_flush_icache(uint64_t flags) {
  if (flags & ~1ull) { return -22; }
  __asm__ volatile("fence.i" ::: "memory");
  if (!flags) { remote_fence(0); }
  return 0;
}
