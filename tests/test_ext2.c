#include "ext2.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void test_large_file_write(struct ext2_fs *fs) {
  struct ext2_node node;
  assert(ext2_create(fs, "/large-write.bin", false, &node));

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
  assert(ext2_create(fs, "/symlink-bin", true, NULL));
  assert(ext2_create(fs, "/symlink-lib", true, NULL));
  assert(ext2_create(fs, "/symlink-lib/llvm", true, NULL));
  assert(ext2_create(fs, "/symlink-lib/llvm/tool-real", false, &node));
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

  assert(ext2_create(&fs, "/apkdir", true, NULL));
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

  assert(fclose(f) == 0);
  assert(remove(tmp_path) == 0);
}

int main(int argc, char **argv) {
  assert(argc == 2);
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

  fclose(f);
  test_mutations(argv[1]);
  return 0;
}
