#include "platform.h"
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
void console_putc(char c) { sbi(DBCN, 2, (unsigned char)c, 0); }
int console_getc(void) {
  if (!input_page) { input_page = pmm_alloc_page(); }
  if (!input_page) { return -1; }
  long n;
  do { n = sbi(DBCN, 1, 1, input_page); } while (n == 0);
  return n == 1 ? *(uint8_t *)pmm_phys_to_virt(input_page) : -1;
}
[[noreturn]] void platform_poweroff(void) {
  sbi(0x53525354, 0, 0, 0);
  for (;;) { __asm__ volatile("wfi"); }
}
