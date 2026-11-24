#include "boot.h"
#include "init/app.h"
#include "lib/kstr.h"

#ifdef SPORE_USERLAND
bool app_mode;
#endif
static uint64_t payload_size;

uint64_t app_payload_size(void) {
  return payload_size;
}

void boot_select_profile(const struct spore_boot_info *boot, struct boot_profile *profile) {
  for (size_t i = 0; i < boot->module_count; ++i) {
    const struct spore_boot_module *module = &boot->modules[i];
    if (str_eq(module->path, "/spore.app")) {
      profile->payload = module;
      payload_size = module->size;
      break;
    }
  }
#ifdef SPORE_USERLAND
  app_mode = profile->payload != NULL;
  if (!app_mode) {
    static const char init_path[] = "/sbin/init";
    profile->program = (struct init_program){.path = init_path, .cwd = "/", .argv = {init_path}, .argc = 1};
  }
#endif
}
