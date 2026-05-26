#include "bootloader.h"

void *memset(void *dst, int c, uint64_t n) {
  uint8_t *d = dst;
  for (uint64_t i = 0; i < n; ++i) {
    d[i] = (uint8_t)c;
  }
  return dst;
}

void *memcpy(void *dst, const void *src, uint64_t n) {
  uint8_t *d = dst;
  const uint8_t *s = src;
  for (uint64_t i = 0; i < n; ++i) {
    d[i] = s[i];
  }
  return dst;
}

uint64_t strlen8(const char *s) {
  uint64_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}

void uefi_puts(const CHAR16 *s) {
  st->con_out->output_string(st->con_out, (CHAR16 *)s);
}

void set_boot_timeout_zero(void) {
  if (st->runtime_services == NULL || st->runtime_services->set_variable == NULL) { return; }
  UINT16 timeout = 0;
  (void)st->runtime_services->set_variable(u"Timeout", (EFI_GUID *)&EFI_GLOBAL_VARIABLE_GUID,
                                           EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                             EFI_VARIABLE_RUNTIME_ACCESS,
                                           sizeof(timeout), &timeout);
}

static int is_leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint64_t unix_days_before_year(int year) {
  uint64_t days = 0;
  for (int y = 1970; y < year; ++y) {
    days += is_leap_year(y) ? 366u : 365u;
  }
  return days;
}

static uint64_t unix_epoch_from_efi_time(const EFI_TIME *time) {
  static const uint8_t month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (time == NULL || time->year < 1970 || time->month < 1 || time->month > 12 || time->day < 1 || time->day > 31) {
    return 0;
  }
  uint64_t days = unix_days_before_year(time->year);
  for (uint8_t m = 1; m < time->month; ++m) {
    days += month_days[m - 1];
    if (m == 2 && is_leap_year(time->year)) { ++days; }
  }
  days += time->day - 1;
  int64_t seconds = (int64_t)(days * 86400ull + (uint64_t)time->hour * 3600ull + (uint64_t)time->minute * 60ull +
                              (uint64_t)time->second);
  if (time->timezone >= -1440 && time->timezone <= 1440) { seconds -= (int64_t)time->timezone * 60; }
  return seconds > 0 ? (uint64_t)seconds : 0;
}

uint64_t read_realtime_epoch(void) {
  if (st->runtime_services == NULL || st->runtime_services->get_time == NULL) { return 0; }
  EFI_TIME time;
  if (EFI_ERROR(st->runtime_services->get_time(&time, NULL))) { return 0; }
  return unix_epoch_from_efi_time(&time);
}

static void uart_putc(char c) {
  volatile uint32_t *uart = (volatile uint32_t *)(uintptr_t)PL011_PHYS;
  volatile uint32_t *fr = (volatile uint32_t *)(uintptr_t)(PL011_PHYS + 0x18);
  while ((*fr & (1u << 5)) != 0) {}
  if (c == '\n') { uart_putc('\r'); }
  *uart = (uint32_t)c;
}

void uart_puts(const char *s) {
  while (*s != '\0') {
    uart_putc(*s++);
  }
}

uint64_t align_down(uint64_t value, uint64_t align) {
  return value & ~(align - 1);
}

uint64_t align_up(uint64_t value, uint64_t align) {
  return (value + align - 1) & ~(align - 1);
}

uint64_t pages_for(uint64_t size) {
  return align_up(size, PAGE_SIZE) / PAGE_SIZE;
}

EFI_STATUS alloc_pages(uint64_t pages, void **out) {
  EFI_PHYSICAL_ADDRESS pa = 0;
  EFI_STATUS status = bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, pages, &pa);
  if (EFI_ERROR(status)) { return status; }
  *out = (void *)(uintptr_t)pa;
  memset(*out, 0, pages * PAGE_SIZE);
  return EFI_SUCCESS;
}
