#include "user.h"
#include "platform.h"
#include "init/boot_info.h"
#include "lib/kstr.h"
#include "mm/pmm.h"

static void puts_serial(const char *s) {
  while (*s) {
    if (*s == '\n') { console_putc('\r'); }
    console_putc(*s++);
  }
}

static void memory(void) {
  char digits[21];
  char *end = digits + sizeof(digits) - 1;
  *end = 0;
  uint64_t kib = pmm_free_pages() * (PAGE_SIZE / 1024);
  do { *--end = '0' + kib % 10; } while ((kib /= 10) != 0);
  puts_serial(end);
  puts_serial(" KiB free\n");
}

[[noreturn]] void port_main(void) {
  console_init();
  const struct spore_boot_info *boot = limine_boot_info();
  if (!boot) { puts_serial("Spore: invalid firmware handoff\n"); platform_poweroff(); }
  pmm_init(boot->hhdm_offset, boot->memmap, boot->memmap_count);
  if (!pmm_free_pages()) { puts_serial("Spore: no usable memory\n"); platform_poweroff(); }
  if (boot->module_count) { puts_serial("[kernel] starting app\n"); user_start(boot); }
  puts_serial("Spore kernel monitor\nCommands: memory, poweroff\n");
  for (;;) {
    char cmd[32];
    size_t n = 0;
    bool overflow = false;
    puts_serial("spore> ");
    for (;;) {
      int c = console_getc();
      if (c < 0) { puts_serial("console unavailable\n"); platform_poweroff(); }
      if (c == '\n' || c == '\r') { break; }
      if (c == 8 || c == 127) {
        if (n) { --n; puts_serial("\b \b"); }
      } else if (c >= 32 && c < 127) {
        if (n + 1 < sizeof(cmd)) { cmd[n++] = c; console_putc(c); }
        else { overflow = true; }
      }
    }
    puts_serial("\n");
    cmd[n] = 0;
    if (overflow) { puts_serial("command too long\n"); }
    else if (str_eq(cmd, "poweroff")) { platform_poweroff(); }
    else if (str_eq(cmd, "memory")) { memory(); }
    else if (n) { puts_serial("Commands: memory, poweroff\n"); }
  }
}
