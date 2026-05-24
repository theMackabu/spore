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
                                          .path = "/init",
                                        }};

  struct ramfs fs;
  struct ramfs_file file;
  ramfs_init(&fs, modules, 2, 0);

  assert(ramfs_lookup(&fs, "/init", &file));
  assert(file.data == init_data);
  assert(file.size == sizeof(init_data));
  assert(strcmp(file.path, "/init") == 0);
  assert(!ramfs_lookup(&fs, "/missing", &file));

  struct ramfs_node node;
  assert(ramfs_lookup_node(&fs, "/", &node));
  assert(node.is_dir);
  assert(ramfs_lookup_node(&fs, "/etc/motd", &node));
  assert(!node.is_dir);
  assert(node.size == 17);
  assert(ramfs_lookup_node(&fs, "/init", &node));
  assert(!node.is_dir);
  assert(node.data == init_data);

  struct ramfs_dirent ent;
  assert(ramfs_root_dirent(0, &ent));
  assert(strcmp(ent.name, "bin") == 0 && ent.is_dir);
  assert(ramfs_root_dirent(1, &ent));
  assert(strcmp(ent.name, "demos") == 0 && ent.is_dir);
  assert(ramfs_root_dirent(2, &ent));
  assert(strcmp(ent.name, "etc") == 0 && ent.is_dir);
  assert(ramfs_root_dirent(3, &ent));
  assert(strcmp(ent.name, "tmp") == 0 && ent.is_dir);
  assert(ramfs_root_dirent(4, &ent));
  assert(strcmp(ent.name, "init") == 0 && !ent.is_dir);

  assert(ramfs_mkdir(&fs, "/tmp/d"));
  assert(ramfs_create(&fs, "/tmp/d/a", &node));
  assert(ramfs_write(&fs, node.index, 0, "hello", 5) == 5);
  char buf[8] = {0};
  assert(ramfs_read(&fs, node.index, 0, buf, sizeof(buf)) == 5);
  assert(strcmp(buf, "hello") == 0);
  assert(ramfs_rename(&fs, "/tmp/d/a", "/tmp/d/b"));
  assert(!ramfs_lookup_node(&fs, "/tmp/d/a", &node));
  assert(ramfs_lookup_node(&fs, "/tmp/d/b", &node));
  assert(ramfs_unlink(&fs, "/tmp/d/b"));
  assert(ramfs_unlink(&fs, "/tmp/d"));
  return 0;
}
