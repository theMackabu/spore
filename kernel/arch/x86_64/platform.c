#include "user.h"
#include "platform.h"
#include <stdint.h>

constexpr uint16_t COM1 = 0x3f8;
constexpr uint16_t COM2 = 0x2f8;
static void outb(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0,%1" :: "a"(value), "Nd"(port));
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
  while (!(inb(port + 5) & 0x20)) { __asm__ volatile("pause"); }
  outb(port, c);
}
void console_init(void) { uart_init(COM1); }
void console_putc(char c) { uart_write(COM1, c); }
int console_getc(void) {
  while (!(inb(COM1 + 5) & 1)) { __asm__ volatile("pause"); }
  return inb(COM1);
}
[[noreturn]] void platform_poweroff(void) {
  __asm__ volatile("outw %0,%1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
  for (;;) { __asm__ volatile("cli; hlt"); }
}

bool status_init(uint64_t hhdm) {
  (void)hhdm;
  uart_init(COM2);
  return inb(COM2 + 5) != 0xff;
}
void status_write(const uint8_t *data, size_t n) {
  for (size_t i = 0; i < n; ++i) { uart_write(COM2, data[i]); }
}
