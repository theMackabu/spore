#include "platform.h"
#include "arch/clock.h"
#include "user.h"
#include <stdint.h>

constexpr uint16_t COM1 = 0x3f8;
constexpr uint16_t COM2 = 0x2f8;
static void outb(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0,%1" ::"a"(value), "Nd"(port));
}
static uint8_t inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
  return value;
}
static void uart_init(uint16_t port) {
  outb(port + 1, 0);
  outb(port + 3, 0x80);
  outb(port, 1);
  outb(port + 1, 0);
  outb(port + 3, 3);
  outb(port + 2, 7);
  outb(port + 4, 3);
}
static void uart_write(uint16_t port, uint8_t c) {
  while (!(inb(port + 5) & 0x20)) {
    __asm__ volatile("pause");
  }
  outb(port, c);
}
void console_init(void) {
  uart_init(COM1);
}
void console_putc(char c) {
  uart_write(COM1, c);
}
int console_getc(void) {
  while (!(inb(COM1 + 5) & 1)) {
    __asm__ volatile("pause");
  }
  return inb(COM1);
}
[[noreturn]] void platform_poweroff(void) {
  __asm__ volatile("outw %0,%1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}

bool status_init(uint64_t hhdm) {
  (void)hhdm;
  uart_init(COM2);
  return inb(COM2 + 5) != 0xff;
}
void status_write(const uint8_t *data, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    uart_write(COM2, data[i]);
  }
}

int console_try_getc(void) {
  return inb(COM1 + 5) & 1 ? inb(COM1) : -1;
}
[[noreturn]] void arch_reboot(void) {
  outb(0xcf9, 6);
  for (;;) {
    __asm__ volatile("hlt");
  }
}

static constexpr uintptr_t apic = 0xffffc00000000000ull;
static bool x2apic;

static void apic_write(unsigned offset, uint32_t value) {
  if (x2apic) {
    __asm__ volatile("wrmsr" ::"c"(0x800u + offset / 16), "a"(value), "d"(0));
  } else {
    *(volatile uint32_t *)(apic + offset) = value;
  }
}

static uint32_t apic_read(unsigned offset) {
  if (x2apic) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x800u + offset / 16));
    return low;
  }
  return *(volatile uint32_t *)(apic + offset);
}

bool x86_map_timer(void) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x1bu));
  x2apic = (low & (1u << 10)) != 0;
  if (!(low & (1u << 11))) { return false; }
  return x2apic || map_page(apic, ((uint64_t)high << 32) | (low & ~4095u), PAGE_READ | PAGE_WRITE | PAGE_DEVICE);
}

void x86_timer_init(void) {
  outb(0x21, 0xff);
  outb(0xa1, 0xff);
  apic_write(0x80, 0);
  apic_write(0xf0, 0x1ff);
  apic_write(0x3e0, 3);
  apic_write(0x320, (1u << 16) | 32);
  uint64_t duration = arch_counter_frequency() / 100;
  apic_write(0x380, UINT32_MAX);
  uint64_t start = arch_counter_value();
  while (arch_counter_value() - start < duration) {
    __asm__ volatile("pause");
  }
  uint32_t period = UINT32_MAX - apic_read(0x390);
  apic_write(0x320, (1u << 17) | 32);
  apic_write(0x380, period);
}
void platform_rearm_timer(void) {
  apic_write(0xb0, 0);
}
void platform_wait_tick(void) {
  __asm__ volatile("sti; hlt; cli" ::: "memory");
}
