#include "init/app.h"
#include "arch/aarch64/platform.h"
#include "drivers/virtio_console.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "proc/limits.h"

bool app_mode;
uint64_t app_payload_bytes;
uint64_t app_entry_pc;
struct app_launch launch = {.path = "/sbin/init", .cwd = "/", .argv = {"/sbin/init"}, .argc = 1};
static const uint8_t *cursor;
static size_t remaining;

void app_detect(const struct spore_boot_info *boot) {
  for (size_t i = 0; i < boot->module_count; ++i) {
    const struct spore_boot_module *m = &boot->modules[i];
    if (str_eq(m->path, "/spore.app")) {
      app_mode = true;
      cursor = (const void *)(uintptr_t)(boot->hhdm_offset + m->phys_addr);
      app_payload_bytes = remaining = m->size;
      return;
    }
  }
}

static const void *take(size_t n) {
  if (n > remaining) { return NULL; }
  const uint8_t *p = cursor;
  cursor += n;
  remaining -= n;
  return p;
}

static const char *string(void) {
  size_t n = 0;
  while (n < remaining && cursor[n]) {
    ++n;
  }
  return n == remaining ? NULL : take(n + 1);
}

static bool path_ok(const char *p) {
  if (!p || *p != '/' || kstrlen(p) >= CELL_PATH_MAX) { return false; }
  while (*p) {
    const char *part = ++p;
    while (*p && *p != '/') {
      ++p;
    }
    size_t n = p - part;
    if (!n || (n == 1 && *part == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) { return false; }
  }
  return true;
}

bool app_unpack(struct ramfs *fs) {
  const char *magic = take(8);
  struct {
    uint32_t argc, envc, entries, ip, gateway, mask, dns;
  } header;
  const void *bytes = take(sizeof(header));
  if (!magic || kmemcmp(magic, "SPOREAP1", 8) || !bytes) { return false; }
  kmemcpy(&header, bytes, sizeof(header));
  launch.argc = header.argc;
  launch.envc = header.envc;
  launch.network = (struct net_config){header.ip, header.gateway, header.mask, header.dns, 1};
  launch.path = string();
  launch.cwd = string();
  if (!path_ok(launch.path) || !launch.cwd || (!str_eq(launch.cwd, "/") && !path_ok(launch.cwd)) || !launch.argc ||
      launch.argc > APP_MAX_ARGS || launch.envc > APP_MAX_ENV) {
    return false;
  }
  for (unsigned i = 0; i < launch.argc; ++i) {
    if (!(launch.argv[i] = string())) { return false; }
  }
  for (unsigned i = 0; i < launch.envc; ++i) {
    if (!(launch.env[i] = string())) { return false; }
  }
  for (unsigned i = 0; i < header.entries; ++i) {
    const char *path = string();
    const void *size = take(8);
    if (!path_ok(path) || !size) { return false; }
    uint64_t n;
    kmemcpy(&n, size, sizeof(n));
    if (n == UINT64_MAX) {
      if (!ramfs_mkdir(fs, path, 0555)) { return false; }
    } else if (n == UINT64_MAX - 1) {
      const char *source = string();
      if (!path_ok(source) || !ramfs_link(fs, source, path)) { return false; }
    } else {
      const void *data = take(n);
      if (!data || !ramfs_add_boot_file(fs, path, data, n, str_eq(path, launch.path) ? 0555 : 0444)) { return false; }
    }
  }
  return remaining == 0;
}

void app_event(char kind, unsigned value) {
  static bool announced;
  if (!announced) {
    static const uint8_t magic[] = {0, 'S', 'P', 'O', 'R', 'E', '1', 0};
    virtio_console_write(magic, sizeof(magic));
    announced = true;
  }
  uint8_t record[2] = {(uint8_t)kind, (uint8_t)value};
  virtio_console_write(record, sizeof(record));
}

void app_finish(char kind, unsigned value) {
  if (!vfs_shutdown()) {
    kind = 'B';
    value = 2;
  }
  app_event(kind, value);
  aarch64_poweroff();
}
