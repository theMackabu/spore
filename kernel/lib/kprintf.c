#include "lib/kprintf.h"

#include "init/app.h"
#include "drivers/serial.h"
#include "drivers/pl011.h"
#include "drivers/virtio_console.h"

#include <stdbool.h>
#include <stddef.h>

static char kernel_log[16384];
static uint64_t kernel_log_head;
static uint64_t kernel_log_len;
static bool kernel_log_handoff;

void klog_handoff(void) { kernel_log_handoff = true; }

static void klog_store(char c) {
  kernel_log[kernel_log_head] = c;
  kernel_log_head = (kernel_log_head + 1u) % sizeof(kernel_log);
  if (kernel_log_len < sizeof(kernel_log)) { ++kernel_log_len; }
}

uint64_t klog_copy(char *dst, uint64_t cap) {
  uint64_t n = kernel_log_len < cap ? kernel_log_len : cap;
  uint64_t start = (kernel_log_head + sizeof(kernel_log) - kernel_log_len) % sizeof(kernel_log);
  for (uint64_t i = 0; i < n; ++i) {
    dst[i] = kernel_log[(start + i) % sizeof(kernel_log)];
  }
  return n;
}

void kputc(char c) {
  klog_store(c);
  if (!app_mode && virtio_console_ready()) {
    virtio_console_write((const uint8_t *)&c, 1);
  } else if (!kernel_log_handoff) {
    if (c == '\n') { serial_putc('\r'); }
    serial_putc(c);
  }
}

void kputs(const char *s) {
  while (*s != '\0') {
    kputc(*s++);
  }
}

static void print_unsigned(uint64_t value, uint64_t base) {
  char buf[32];
  size_t i = 0;

  if (value == 0) {
    kputc('0');
    return;
  }

  while (value != 0) {
    uint64_t digit = value % base;
    buf[i++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
    value /= base;
  }
  while (i > 0) {
    kputc(buf[--i]);
  }
}

static void print_signed(int64_t value) {
  if (value < 0) {
    kputc('-');
    print_unsigned((uint64_t)-value, 10);
    return;
  }
  print_unsigned((uint64_t)value, 10);
}

void kprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  for (const char *p = fmt; *p != '\0'; ++p) {
    if (*p != '%') {
      kputc(*p);
      continue;
    }

    char spec = *++p;
    switch (spec) {
    case '%':
      kputc('%');
      break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      kputs(s == NULL ? "(null)" : s);
      break;
    }
    case 'd':
      print_signed((int64_t)va_arg(ap, int));
      break;
    case 'u':
      print_unsigned((uint64_t)va_arg(ap, unsigned int), 10);
      break;
    case 'x':
      print_unsigned((uint64_t)va_arg(ap, unsigned int), 16);
      break;
    case 'p':
      kputs("0x");
      print_unsigned((uint64_t)(uintptr_t)va_arg(ap, void *), 16);
      break;
    default:
      kputc('%');
      kputc(spec);
      break;
    }
  }

  va_end(ap);
}

void klog_init_console(uint64_t hhdm_offset, unsigned cpus, const char *loader) {
  pl011_init(hhdm_offset);
  kprintf("[kernel] %s handoff at EL1, %u CPU(s)\n", loader, cpus);
  klog_handoff();
  (void)virtio_console_init(hhdm_offset);
}
