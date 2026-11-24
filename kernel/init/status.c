#include "boot.h"
#include "init/app.h"
#include "init/event.h"
#include "arch/aarch64/platform.h"
#include "drivers/virtio_console.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"

static uint64_t entry_pc;

void app_track_entry(uint64_t pc) {
  entry_pc = pc;
}

void app_instruction_fetched(uint64_t pc) {
  if (entry_pc && entry_pc == pc) {
    entry_pc = 0;
    app_event(virtio_console_write, 'U', 0);
  }
}

void app_finish(enum app_result kind, unsigned value) {
  if (!vfs_shutdown()) {
    kind = APP_BOOT_ERROR;
    value = BOOT_FLUSH_FAILED;
  }
  app_event(virtio_console_write, kind, value);
  aarch64_poweroff();
}

void boot_fail(enum boot_error error, const char *reason) {
  kprintf("[kernel] %s\n", reason);
  if (app_mode) { app_finish(APP_BOOT_ERROR, error); }
  aarch64_halt();
}
