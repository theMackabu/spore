#include "ext2.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool file_read(void *ctx, uint64_t offset, void *dst, uint32_t len) {
  FILE *f = ctx;
  if (fseek(f, (long)offset, SEEK_SET) != 0) { return false; }
  return fread(dst, 1, len, f) == len;
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
  assert(strcmp(buf, "welcome to spore\n") == 0);

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
  return 0;
}
