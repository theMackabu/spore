#include "ramfs.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
  const char init_data[] = "init";
  struct spore_boot_module modules[] = {{
                                          .phys_addr = (uint64_t)(uintptr_t)"x",
                                          .size = 1,
                                          .path = "/other",
                                        },
                                        {
                                          .phys_addr = (uint64_t)(uintptr_t)init_data,
                                          .size = sizeof(init_data),
                                          .path = "/sbin/init",
                                        }};

  struct ramfs fs;
  struct ramfs_file file;
  ramfs_init(&fs, modules, 2, 0);

  assert(ramfs_lookup(&fs, "/sbin/init", &file));
  assert(file.data == init_data);
  assert(file.size == sizeof(init_data));
  assert(strcmp(file.path, "/sbin/init") == 0);
  assert(!ramfs_lookup(&fs, "/missing", &file));

  struct ramfs_node node;
  assert(ramfs_lookup_node(&fs, "/", &node));
  assert(node.is_dir);
  assert(ramfs_lookup_node(&fs, "/sbin/init", &node));
  assert(!node.is_dir);
  assert(node.data == init_data);

  struct ramfs_dirent ent;
  assert(ramfs_root_dirent(0, &ent));
  assert(strcmp(ent.name, "dev") == 0 && ent.is_dir);
  assert(ramfs_root_dirent(1, &ent));
  assert(strcmp(ent.name, "proc") == 0 && ent.is_dir);
  assert(ramfs_root_dirent(2, &ent));
  assert(strcmp(ent.name, "sys") == 0 && ent.is_dir);
  assert(ramfs_root_dirent(3, &ent));
  assert(strcmp(ent.name, "tmp") == 0 && ent.is_dir);
  assert(ramfs_root_dirent(4, &ent));
  assert(strcmp(ent.name, "run") == 0 && ent.is_dir);
  assert(ramfs_lookup_node(&fs, "/dev/null", &node));
  assert(!node.is_dir && node.device == RAMFS_DEV_NULL);
  assert(ramfs_lookup_node(&fs, "/dev/zero", &node));
  assert(!node.is_dir && node.device == RAMFS_DEV_ZERO);
  assert(ramfs_lookup_node(&fs, "/dev/fs/root", &node));
  assert(!node.is_dir && node.device == RAMFS_DEV_FS_ROOT);
  assert(ramfs_lookup_node(&fs, "/dev/fs/tmp", &node));
  assert(!node.is_dir && node.device == RAMFS_DEV_FS_TMP);
  assert(ramfs_lookup_node(&fs, "/dev/blk/root", &node));
  assert(!node.is_dir && node.device == RAMFS_DEV_BLK_ROOT);
  assert(ramfs_lookup_node(&fs, "/proc/procinfo", &node));
  assert(!node.is_dir && node.device == RAMFS_DEV_PROCINFO);
  assert(ramfs_lookup_node(&fs, "/proc/filesystems", &node));
  assert(!node.is_dir && node.device == RAMFS_DEV_FILESYSTEMS);
  assert(ramfs_lookup_node(&fs, "/proc/partitions", &node));
  assert(!node.is_dir && node.device == RAMFS_DEV_PARTITIONS);
  assert(ramfs_lookup_node(&fs, "/proc/devices", &node));
  assert(!node.is_dir && node.device == RAMFS_DEV_DEVICES);
  assert(ramfs_lookup_node(&fs, "/sys/devices/system/cpu/online", &node));
  assert(!node.is_dir && node.device == RAMFS_DEV_SYS_CPU_ONLINE);
  assert(ramfs_lookup_node(&fs, "/sys/devices/system/cpu/cpu0/online", &node));
  assert(!node.is_dir && node.device == RAMFS_DEV_SYS_CPU_CORE_ONLINE);
  assert(ramfs_lookup_node(&fs, "/dev/procinfo", &node));
  assert(!node.is_dir && node.device == RAMFS_DEV_PROCINFO);

  assert(ramfs_mkdir(&fs, "/tmp/d"));
  assert(ramfs_mkfifo(&fs, "/run/f", 0666, &node));
  assert((node.mode & 0170000u) == 0010000u);
  assert(ramfs_mksock(&fs, "/run/s", 0666, &node));
  assert((node.mode & 0170000u) == 0140000u);
  assert(ramfs_create(&fs, "/tmp/d/a", &node));
  assert(ramfs_write(&fs, node.index, 0, "hello", 5) == 5);
  assert(ramfs_chmod_node(&fs, node.index, 0600));
  assert(ramfs_refresh_node(&fs, node.index, &node));
  assert(node.mode == 0600);
  char buf[8] = {0};
  assert(ramfs_read(&fs, node.index, 0, buf, sizeof(buf)) == 5);
  assert(strcmp(buf, "hello") == 0);
  assert(ramfs_link(&fs, "/tmp/d/a", "/tmp/d/link"));
  memset(buf, 0, sizeof(buf));
  assert(ramfs_lookup_node(&fs, "/tmp/d/link", &node));
  assert(ramfs_read(&fs, node.index, 0, buf, sizeof(buf)) == 5);
  assert(strcmp(buf, "hello") == 0);
  assert(ramfs_unlink(&fs, "/tmp/d/link"));
  assert(ramfs_lookup_node(&fs, "/tmp/d/a", &node));
  uint64_t sparse_tail = 256ull * 1024ull * 1024ull;
  assert(ramfs_write(&fs, node.index, sparse_tail, "tail", 4) == 4);
  memset(buf, 0xff, sizeof(buf));
  assert(ramfs_read(&fs, node.index, 8192, buf, sizeof(buf)) == sizeof(buf));
  for (size_t i = 0; i < sizeof(buf); ++i) {
    assert(buf[i] == 0);
  }
  memset(buf, 0, sizeof(buf));
  assert(ramfs_read(&fs, node.index, sparse_tail, buf, 4) == 4);
  assert(memcmp(buf, "tail", 4) == 0);
  assert(ramfs_truncate(&fs, node.index, 5));
  assert(ramfs_rename(&fs, "/tmp/d/a", "/tmp/d/b"));
  assert(!ramfs_lookup_node(&fs, "/tmp/d/a", &node));
  assert(ramfs_lookup_node(&fs, "/tmp/d/b", &node));
  assert(ramfs_unlink(&fs, "/tmp/d/b"));
  assert(ramfs_unlink(&fs, "/tmp/d"));
  return 0;
}
