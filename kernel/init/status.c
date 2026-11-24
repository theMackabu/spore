#include "boot.h"
#include "init/app.h"
#include "arch/aarch64/platform.h"
#include "drivers/virtio_console.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"

static uint64_t entry_pc;

static void app_event(char kind, unsigned value) {
  static bool announced;
  if (!announced) {
    static const uint8_t magic[] = {0, 'S', 'P', 'O', 'R', 'E', '1', 0};
    virtio_console_write(magic, sizeof(magic));
    announced = true;
  }
  uint8_t record[2] = {(uint8_t)kind, (uint8_t)value};
  virtio_console_write(record, sizeof(record));
}

void app_track_entry(uint64_t pc) {
  entry_pc = pc;
}

void app_instruction_fetched(uint64_t pc) {
  if (entry_pc && entry_pc == pc) {
    entry_pc = 0;
    app_event('U', 0);
  }
}

void app_finish(enum app_result kind, unsigned value) {
  if (!vfs_shutdown()) {
    kind = APP_BOOT_ERROR;
    value = BOOT_FLUSH_FAILED;
  }
  app_event(kind, value);
  aarch64_poweroff();
}

void boot_fail(enum boot_error error, const char *reason) {
  kprintf("[kernel] %s\n", reason);
  if (app_mode) { app_finish(APP_BOOT_ERROR, error); }
  aarch64_halt();
}
