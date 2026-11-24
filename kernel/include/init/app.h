#pragma once

#include "init/boot_info.h"
#include "net/net.h"

struct ramfs;

enum { APP_MAX_ARGS = 128, APP_MAX_ENV = 128 };

struct app_launch {
  const char *path, *cwd;
  const char *argv[APP_MAX_ARGS], *env[APP_MAX_ENV];
  unsigned argc, envc;
  struct net_config network;
};

extern bool app_mode;
extern uint64_t app_entry_pc, app_payload_bytes;
extern struct app_launch launch;

void app_detect(const struct spore_boot_info *boot);
bool app_unpack(struct ramfs *fs);
void app_event(char kind, unsigned value);
[[noreturn]] void app_finish(char kind, unsigned value);
