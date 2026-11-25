#include "boot.h"
#include "drivers/virtio_blk.h"
#include "fs/ext2.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "init/app.h"
#include "lib/kprintf.h"
#include "net/net.h"

static bool disk_read(void *ctx, uint64_t offset, void *dst, uint32_t len) {
  (void)ctx;
  return virtio_blk_read(offset, dst, len);
}

static bool disk_write(void *ctx, uint64_t offset, const void *src, uint32_t len) {
  (void)ctx;
  return virtio_blk_write(offset, src, len);
}

struct ext2_fs *boot_mount_disk(uint64_t hhdm_offset) {
  static struct ext2_fs disk;
  if (!virtio_blk_init(hhdm_offset)) { return NULL; }
  if (ext2_mount_rw(&disk, disk_read, disk_write, NULL)) { return &disk; }
  if (app_mode) { boot_fail(BOOT_INIT_FAILED, "cannot mount data volume"); }
  kprintf("[spore] ext2: mount failed\n");
  return NULL;
}

void boot_mount_root(const struct spore_boot_info *boot, struct ext2_fs *disk, struct boot_profile *profile) {
  static struct ramfs root;
  if (!ramfs_init(&root, boot->modules, app_mode ? 0 : boot->module_count, boot->hhdm_offset)) {
    boot_fail(BOOT_INIT_FAILED, "ramfs initialization failed");
  }
  if (app_mode) {
    const struct spore_boot_module *payload = profile->payload;
    if (!payload) { boot_fail(BOOT_INVALID_PAYLOAD, "app payload missing"); }
    const void *data = (const void *)(uintptr_t)(boot->hhdm_offset + payload->phys_addr);
    struct net_config network;
    if (!app_unpack(data, payload->size, &root, &profile->program, &network)) {
      boot_fail(BOOT_INVALID_PAYLOAD, "invalid app payload");
    }
    net_set_config(&network);
    if (disk && !ramfs_mkdir(&root, 0, "/data", 0555)) { boot_fail(BOOT_DATA_MOUNT_FAILED, "cannot create /data"); }
  }
  vfs_init(&root, disk);
}
