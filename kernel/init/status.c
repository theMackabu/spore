#include "boot.h"
#include "init/app.h"
#include "init/event.h"
#include "arch/platform.h"
#ifdef SPORE_NATIVE_PORT
#include "arch/bringup/user.h"
#define write_status status_write
#else
#include "drivers/virtio_console.h"
#define write_status virtio_console_write
#endif
#include "fs/vfs.h"
#include "lib/kprintf.h"

static uint64_t entry_pc;

void app_track_entry(uint64_t pc) {
  entry_pc = pc;
}

void app_instruction_fetched(uint64_t pc) {
  if (entry_pc && entry_pc == pc) {
    entry_pc = 0;
    app_event(write_status, 'U', 0);
  }
}

void app_finish(enum app_result kind, unsigned value) {
  if (!vfs_shutdown()) {
    kind = APP_BOOT_ERROR;
    value = BOOT_FLUSH_FAILED;
  }
  app_event(write_status, kind, value);
  arch_poweroff();
}

void boot_fail(enum boot_error error, const char *reason) {
  kprintf("[kernel] %s\n", reason);
  if (app_mode) { app_finish(APP_BOOT_ERROR, error); }
  arch_halt();
}
