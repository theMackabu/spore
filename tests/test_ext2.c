#include "ext2.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
  EXT2_SUPER_OFFSET = 1024,
  EXT2_FEATURE_INCOMPAT_OFFSET = EXT2_SUPER_OFFSET + 96,
  EXT2_INCOMPAT_SPORE_EXT2PLUS = 0x80000000u,
  EXT2PLUS_MAGIC = 0x4e495053u,
  EXT2PLUS_VERSION = 1,
  EXT2PLUS_BASE_INO = 0x40000000u,
  EXT2PLUS_CHUNK_INODES = 256,
  EXT2PLUS_FREE_NONE = UINT32_MAX,
};

struct ext2plus_header {
  uint32_t magic;
  uint32_t version;
  uint32_t header_size;
  uint32_t inode_size;
  uint32_t base_ino;
  uint32_t capacity;
  uint32_t next_index;
  uint32_t free_head;
  uint32_t free_count;
  uint32_t chunk_size;
  uint32_t reserved[6];
};

static bool file_read(void *ctx, uint64_t offset, void *dst, uint32_t len) {
  FILE *f = ctx;
  if (fseek(f, (long)offset, SEEK_SET) != 0) { return false; }
  return fread(dst, 1, len, f) == len;
}

static bool file_write(void *ctx, uint64_t offset, const void *src, uint32_t len) {
  FILE *f = ctx;
  if (fseek(f, (long)offset, SEEK_SET) != 0) { return false; }
  return fwrite(src, 1, len, f) == len && fflush(f) == 0;
}

static void copy_file(const char *src_path, const char *dst_path) {
  FILE *src = fopen(src_path, "rb");
  assert(src != NULL);
  FILE *dst = fopen(dst_path, "wb");
  assert(dst != NULL);
  char buf[16384];
  for (;;) {
    size_t n = fread(buf, 1, sizeof(buf), src);
    if (n > 0) { assert(fwrite(buf, 1, n, dst) == n); }
    if (n < sizeof(buf)) {
      assert(feof(src));
      break;
    }
  }
  assert(fclose(src) == 0);
  assert(fclose(dst) == 0);
}

static void assert_readlink(struct ext2_fs *fs, const char *path, const char *want) {
  char got[128];
  size_t len = 0;
  assert(ext2_readlink(fs, path, got, sizeof(got), &len));
  assert(len == strlen(want));
  assert(strcmp(got, want) == 0);
}

static uint32_t read_le32(const uint8_t bytes[4]) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_le32(uint8_t bytes[4], uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
  bytes[2] = (uint8_t)(value >> 16);
  bytes[3] = (uint8_t)(value >> 24);
}

static void set_ext2plus_feature(const char *path) {
  FILE *f = fopen(path, "r+b");
  assert(f != NULL);
  assert(fseek(f, EXT2_FEATURE_INCOMPAT_OFFSET, SEEK_SET) == 0);
  uint8_t bytes[4];
  assert(fread(bytes, 1, sizeof(bytes), f) == sizeof(bytes));
  write_le32(bytes, read_le32(bytes) | EXT2_INCOMPAT_SPORE_EXT2PLUS);
  assert(fseek(f, EXT2_FEATURE_INCOMPAT_OFFSET, SEEK_SET) == 0);
  assert(fwrite(bytes, 1, sizeof(bytes), f) == sizeof(bytes));
  assert(fclose(f) == 0);
}

static void write_ext2plus_seed(const char *root_dir) {
  char path[256];
  snprintf(path, sizeof(path), "%s/.spore-inodes", root_dir);
  FILE *f = fopen(path, "wb");
  assert(f != NULL);
  uint8_t block[4096] = {0};
  struct ext2plus_header hdr = {
    .magic = EXT2PLUS_MAGIC,
    .version = EXT2PLUS_VERSION,
    .header_size = sizeof(hdr),
    .inode_size = 256,
    .base_ino = EXT2PLUS_BASE_INO,
    .free_head = EXT2PLUS_FREE_NONE,
    .chunk_size = EXT2PLUS_CHUNK_INODES,
  };
  memcpy(block, &hdr, sizeof(hdr));
  assert(fwrite(block, 1, sizeof(block), f) == sizeof(block));
  assert(fclose(f) == 0);
}

static void make_ext2_image(char *path, size_t path_cap, uint32_t block_size, uint32_t blocks,
                            uint32_t blocks_per_group) {
  snprintf(path, path_cap, "/tmp/spore-ext2-fresh-%ld.img", (long)getpid());
  unlink(path);
  char cmd[512];
  if (blocks_per_group != 0) {
    snprintf(cmd, sizeof(cmd), "mke2fs -q -t ext2 -b %u -g %u %s %u", block_size, blocks_per_group, path, blocks);
  } else {
    snprintf(cmd, sizeof(cmd), "mke2fs -q -t ext2 -b %u %s %u", block_size, path, blocks);
  }
  assert(system(cmd) == 0);
}

static void make_ext2plus_image(char *path, size_t path_cap) {
  char root_dir[256];
  snprintf(root_dir, sizeof(root_dir), "/tmp/spore-ext2plus-root-%ld", (long)getpid());
  assert(mkdir(root_dir, 0700) == 0);
  write_ext2plus_seed(root_dir);
  snprintf(path, path_cap, "/tmp/spore-ext2plus-%ld.img", (long)getpid());
  unlink(path);
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "mke2fs -q -t ext2 -b 4096 -N 32 -d %s %s 2048", root_dir, path);
  assert(system(cmd) == 0);
  set_ext2plus_feature(path);
  char seed_path[256];
  snprintf(seed_path, sizeof(seed_path), "%s/.spore-inodes", root_dir);
  assert(remove(seed_path) == 0);
  assert(rmdir(root_dir) == 0);
}

static void test_block_group_boundary(uint32_t block_size, uint32_t image_blocks, uint32_t blocks_per_group,
                                      uint32_t write_blocks) {
  char path[256];
  make_ext2_image(path, sizeof(path), block_size, image_blocks, blocks_per_group);
  FILE *f = fopen(path, "r+b");
  assert(f != NULL);

  struct ext2_fs fs;
  assert(ext2_mount_rw(&fs, file_read, file_write, f));
  assert(fs.block_size == block_size);

  struct ext2_node node;
  assert(ext2_create(&fs, "/group-cross.bin", false, 0755, &node));

  uint8_t block[4096];
  assert(block_size <= sizeof(block));
  for (uint32_t i = 0; i < write_blocks; ++i) {
    memset(block, (int)(i & 0xffu), block_size);
    assert(ext2_write_file(&fs, &node, (uint64_t)i * block_size, block, block_size) == (int64_t)block_size);
  }

  struct ext2_node fresh;
  assert(ext2_lookup(&fs, "/group-cross.bin", &fresh));
  assert(fresh.size == write_blocks * block_size);

  uint8_t got[4096];
  uint32_t read = 0;
  assert(ext2_read_file(&fs, &fresh, 100u * block_size, got, block_size, &read));
  assert(read == block_size);
  for (size_t i = 0; i < block_size; ++i) {
    assert(got[i] == (uint8_t)100);
  }

  uint32_t late_block = write_blocks - 200;
  assert(ext2_read_file(&fs, &fresh, (uint64_t)late_block * block_size, got, block_size, &read));
  assert(read == block_size);
  for (size_t i = 0; i < block_size; ++i) {
    assert(got[i] == (uint8_t)(late_block & 0xffu));
  }

  assert(ext2_drop_cache(&fs));
  assert(fclose(f) == 0);
  assert(remove(path) == 0);
}

static void test_ext2plus_dynamic_inodes(void) {
  char path[256];
  make_ext2plus_image(path, sizeof(path));
  FILE *f = fopen(path, "r+b");
  assert(f != NULL);

  struct ext2_fs fs;
  assert(ext2_mount_rw(&fs, file_read, file_write, f));
  assert(fs.ext2plus);
  struct ext2_node hidden;
  assert(!ext2_lookup(&fs, "/.spore-inodes", &hidden));
  struct ext2_node root;
  assert(ext2_lookup(&fs, "/", &root));
  struct ext2_dirent hidden_ent;
  for (size_t i = 0; ext2_dirent(&fs, &root, i, &hidden_ent); ++i) {
    assert(strcmp(hidden_ent.name, ".spore-inodes") != 0);
  }

  struct ext2_info info;
  assert(ext2_info(&fs, &info));
  assert(info.inode_count == 32);

  struct ext2_node first;
  assert(ext2_create(&fs, "/first-dynamic", false, 0755, &first));
  assert(first.ino >= EXT2PLUS_BASE_INO);
  const char payload[] = "stored through ext2+\n";
  assert(ext2_write_file(&fs, &first, 0, payload, sizeof(payload) - 1) == (int64_t)(sizeof(payload) - 1));
  assert(ext2_info(&fs, &info));
  assert(info.inode_count == 32 + EXT2PLUS_CHUNK_INODES);

  uint32_t reused_ino = 0;
  for (int i = 0; i < 270; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "/dyn-%03d", i);
    struct ext2_node node;
    assert(ext2_create(&fs, name, false, 0644, &node));
    assert(node.ino >= EXT2PLUS_BASE_INO);
    if (i == 0) { reused_ino = node.ino; }
  }
  assert(ext2_info(&fs, &info));
  assert(info.inode_count == 32 + 2 * EXT2PLUS_CHUNK_INODES);

  assert(ext2_unlink(&fs, "/dyn-000"));
  struct ext2_node reused;
  assert(ext2_create(&fs, "/reused-dynamic", false, 0644, &reused));
  assert(reused.ino == reused_ino);

  assert(ext2_drop_cache(&fs));
  assert(fclose(f) == 0);

  f = fopen(path, "rb");
  assert(f != NULL);
  assert(ext2_mount(&fs, file_read, f));
  assert(!ext2_lookup(&fs, "/.spore-inodes", &hidden));
  struct ext2_node fresh;
  assert(ext2_lookup(&fs, "/first-dynamic", &fresh));
  assert(fresh.ino == first.ino);
  char got[sizeof(payload)] = {0};
  uint32_t read = 0;
  assert(ext2_read_file(&fs, &fresh, 0, got, sizeof(payload) - 1, &read));
  assert(read == sizeof(payload) - 1);
  assert(strcmp(got, payload) == 0);
  assert(ext2_lookup(&fs, "/reused-dynamic", &fresh));
  assert(fresh.ino == reused_ino);
  assert(ext2_drop_cache(&fs));
  assert(fclose(f) == 0);
  assert(remove(path) == 0);
}

static void test_large_file_write(struct ext2_fs *fs) {
  struct ext2_node node;
  assert(ext2_create(fs, "/large-write.bin", false, 0755, &node));

  uint32_t entries_per_block = fs->block_size / sizeof(uint32_t);
  uint64_t double_indirect_offset = (12ull + entries_per_block) * fs->block_size;
  const char payload[] = "crossed into double indirect allocation\n";

  int64_t wrote = ext2_write_file(fs, &node, double_indirect_offset, payload, sizeof(payload) - 1);
  assert(wrote == (int64_t)(sizeof(payload) - 1));

  struct ext2_node fresh;
  assert(ext2_lookup(fs, "/large-write.bin", &fresh));
  assert(fresh.size == double_indirect_offset + sizeof(payload) - 1);

  char got[sizeof(payload)] = {0};
  uint32_t read = 0;
  assert(ext2_read_file(fs, &fresh, double_indirect_offset, got, sizeof(payload) - 1, &read));
  assert(read == sizeof(payload) - 1);
  assert(strcmp(got, payload) == 0);

  assert(ext2_unlink(fs, "/large-write.bin"));
}

static void test_relative_symlink_with_dotdot(struct ext2_fs *fs) {
  struct ext2_node node;
  assert(ext2_create(fs, "/symlink-bin", true, 0755, NULL));
  assert(ext2_create(fs, "/symlink-lib", true, 0755, NULL));
  assert(ext2_create(fs, "/symlink-lib/llvm", true, 0755, NULL));
  assert(ext2_create(fs, "/symlink-lib/llvm/tool-real", false, 0755, &node));
  const char payload[] = "real tool\n";
  assert(ext2_write_file(fs, &node, 0, payload, sizeof(payload) - 1) == (int64_t)(sizeof(payload) - 1));
  assert(ext2_symlink(fs, "../symlink-lib/llvm/tool", "/symlink-bin/tool"));
  assert(ext2_symlink(fs, "tool-real", "/symlink-lib/llvm/tool"));

  struct ext2_node resolved;
  assert(ext2_lookup(fs, "/symlink-bin/tool", &resolved));
  assert(ext2_is_regular(&resolved));
  assert(resolved.size == sizeof(payload) - 1);
}

static void test_mutations(const char *image_path) {
  char tmp_path[256];
  snprintf(tmp_path, sizeof(tmp_path), "/tmp/spore-ext2-test-%ld.img", (long)getpid());
  copy_file(image_path, tmp_path);

  FILE *f = fopen(tmp_path, "r+b");
  assert(f != NULL);
  struct ext2_fs fs;
  assert(ext2_mount_rw(&fs, file_read, file_write, f));
  test_large_file_write(&fs);
  test_relative_symlink_with_dotdot(&fs);

  assert(ext2_create(&fs, "/apkdir", true, 0755, NULL));
  for (int i = 0; i < 128; ++i) {
    char tmp[128];
    char final[128];
    snprintf(tmp, sizeof(tmp), "/apkdir/.apk.%048d", i);
    snprintf(final, sizeof(final), "/apkdir/git-tool-%03d", i);
    assert(ext2_symlink(&fs, "../../bin/git", tmp));
    assert(ext2_rename(&fs, tmp, final));
    struct ext2_node node;
    assert(!ext2_lstat(&fs, tmp, &node));
    assert(ext2_lstat(&fs, final, &node));
    assert(ext2_is_symlink(&node));
    assert_readlink(&fs, final, "../../bin/git");
  }

  assert(ext2_symlink(&fs, "old", "/apkdir/.apk.replace"));
  assert(ext2_rename(&fs, "/apkdir/.apk.replace", "/apkdir/replace"));
  assert_readlink(&fs, "/apkdir/replace", "old");
  for (int i = 0; i < 32; ++i) {
    char target[32];
    snprintf(target, sizeof(target), "new-%02d", i);
    assert(ext2_symlink(&fs, target, "/apkdir/.apk.replace"));
    assert(ext2_rename(&fs, "/apkdir/.apk.replace", "/apkdir/replace"));
    assert_readlink(&fs, "/apkdir/replace", target);
    struct ext2_node node;
    assert(!ext2_lstat(&fs, "/apkdir/.apk.replace", &node));
  }

  assert(ext2_drop_cache(&fs));
  assert(fclose(f) == 0);
  assert(remove(tmp_path) == 0);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  test_block_group_boundary(1024, 20000, 0, 8500);
  test_block_group_boundary(4096, 8192, 2048, 3000);
  test_ext2plus_dynamic_inodes();

  FILE *f = fopen(argv[1], "rb");
  assert(f != NULL);

  struct ext2_fs fs;
  assert(ext2_mount(&fs, file_read, f));
  assert(fs.block_size == 1024 || fs.block_size == 4096);

  struct ext2_node root;
  assert(ext2_lookup(&fs, "/", &root));
  assert(ext2_is_dir(&root));

  struct ext2_node motd;
  assert(ext2_lookup(&fs, "/etc/motd", &motd));
  assert(ext2_is_regular(&motd));
  char buf[64] = {0};
  uint32_t got = 0;
  assert(ext2_read_file(&fs, &motd, 0, buf, sizeof(buf) - 1, &got));
  assert(got > 0);
  assert(strstr(buf, "Spore") != NULL || strstr(buf, "spore") != NULL);

  struct ext2_node bin;
  assert(ext2_lookup(&fs, "/bin", &bin));
  assert(ext2_is_dir(&bin));
  bool saw_sh = false;
  bool saw_hello = false;
  struct ext2_dirent ent;
  for (size_t i = 0; ext2_dirent(&fs, &bin, i, &ent); ++i) {
    if (strcmp(ent.name, "sh") == 0) { saw_sh = true; }
    if (strcmp(ent.name, "hello") == 0) { saw_hello = true; }
  }
  assert(saw_sh && saw_hello);

  struct ext2_node hello;
  assert(ext2_lookup(&fs, "/bin/hello", &hello));
  assert(ext2_is_regular(&hello));
  assert(hello.size < 256 * 1024);

  struct ext2_node loader;
  assert(ext2_lookup(&fs, "/lib/ld-musl-aarch64.so.1", &loader));
  assert(ext2_is_regular(&loader));
  assert(loader.size > 512 * 1024);

  struct ext2_node libc;
  assert(ext2_lookup(&fs, "/lib/libc.so", &libc));
  assert(ext2_is_regular(&libc));

  assert(ext2_drop_cache(&fs));
  fclose(f);
  test_mutations(argv[1]);
  return 0;
}
