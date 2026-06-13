#include "ext2.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
  DEFAULT_FILES = 2048,
  IMAGE_BLOCKS = 32768,
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
  uint32_t static_inodes_used;
  uint32_t reserved[5];
};

struct timing {
  uint64_t create_ns;
  uint64_t lookup_ns;
  uint64_t unlink_ns;
  uint32_t first_ino;
  uint32_t last_ino;
  uint64_t start_inodes;
  uint64_t end_inodes;
  uint64_t start_free_inodes;
  uint64_t end_free_inodes;
  uint32_t dynamic_inodes;
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

static uint64_t now_ns(void) {
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

static void make_image(char *path, size_t path_cap, bool ext2plus, unsigned files) {
  char root_dir[256];
  snprintf(root_dir, sizeof(root_dir), "/tmp/spore-bench-root-%ld-%d", (long)getpid(), ext2plus ? 1 : 0);
  assert(mkdir(root_dir, 0700) == 0);
  if (ext2plus) { write_ext2plus_seed(root_dir); }

  snprintf(path, path_cap, "/tmp/spore-bench-%ld-%d.ext2", (long)getpid(), ext2plus ? 1 : 0);
  unlink(path);
  char cmd[512];
  if (ext2plus) {
    snprintf(cmd, sizeof(cmd), "mke2fs -q -t ext2 -b 4096 -N 32 -d %s %s %u", root_dir, path, IMAGE_BLOCKS);
  } else {
    snprintf(cmd, sizeof(cmd), "mke2fs -q -t ext2 -b 4096 -N %u -d %s %s %u", files + 64, root_dir, path,
             IMAGE_BLOCKS);
  }
  assert(system(cmd) == 0);
  if (ext2plus) { set_ext2plus_feature(path); }

  char seed_path[256];
  snprintf(seed_path, sizeof(seed_path), "%s/.spore-inodes", root_dir);
  if (ext2plus) { assert(remove(seed_path) == 0); }
  assert(rmdir(root_dir) == 0);
}

static struct timing run_case(const char *label, bool want_ext2plus, unsigned files) {
  char path[256];
  make_image(path, sizeof(path), want_ext2plus, files);
  FILE *f = fopen(path, "r+b");
  assert(f != NULL);

  struct ext2_fs fs;
  assert(ext2_mount_rw(&fs, file_read, file_write, f));
  assert(fs.ext2plus == want_ext2plus);

  struct ext2_info before;
  assert(ext2_info(&fs, &before));
  struct timing t = {
    .start_inodes = before.inode_count,
    .start_free_inodes = before.free_inodes,
  };

  uint64_t start = now_ns();
  for (unsigned i = 0; i < files; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "/bench-%05u", i);
    struct ext2_node node;
    assert(ext2_create(&fs, name, false, 0644, &node));
    if (i == 0) { t.first_ino = node.ino; }
    if (i == files - 1) { t.last_ino = node.ino; }
    if (node.ino >= EXT2PLUS_BASE_INO) { ++t.dynamic_inodes; }
    const char payload[] = "x\n";
    assert(ext2_write_file(&fs, &node, 0, payload, sizeof(payload) - 1) == (int64_t)(sizeof(payload) - 1));
  }
  assert(ext2_flush(&fs));
  t.create_ns = now_ns() - start;

  start = now_ns();
  for (unsigned i = 0; i < files; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "/bench-%05u", i);
    struct ext2_node node;
    assert(ext2_lookup(&fs, name, &node));
    if (!want_ext2plus) { assert(node.ino < EXT2PLUS_BASE_INO); }
  }
  t.lookup_ns = now_ns() - start;

  start = now_ns();
  for (unsigned i = 0; i < files; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "/bench-%05u", i);
    assert(ext2_unlink(&fs, name));
  }
  assert(ext2_flush(&fs));
  t.unlink_ns = now_ns() - start;

  struct ext2_info after;
  assert(ext2_info(&fs, &after));
  t.end_inodes = after.inode_count;
  t.end_free_inodes = after.free_inodes;

  assert(ext2_drop_cache(&fs));
  assert(fclose(f) == 0);
  assert(remove(path) == 0);

  printf("%-8s files=%u first_ino=%u last_ino=%u dynamic=%u inode_total=%llu->%llu free=%llu->%llu\n", label, files,
         t.first_ino, t.last_ino, t.dynamic_inodes, (unsigned long long)t.start_inodes,
         (unsigned long long)t.end_inodes, (unsigned long long)t.start_free_inodes,
         (unsigned long long)t.end_free_inodes);
  printf("         create+write=%8.3f ms  lookup=%8.3f ms  unlink=%8.3f ms\n", (double)t.create_ns / 1000000.0,
         (double)t.lookup_ns / 1000000.0, (double)t.unlink_ns / 1000000.0);
  return t;
}

int main(int argc, char **argv) {
  unsigned files = DEFAULT_FILES;
  if (argc > 2) {
    fprintf(stderr, "usage: %s [FILE_COUNT]\n", argv[0]);
    return EXIT_FAILURE;
  }
  if (argc == 2) {
    char *end = NULL;
    unsigned long value = strtoul(argv[1], &end, 10);
    if (argv[1][0] == '\0' || end == NULL || *end != '\0' || value == 0 || value > 20000) {
      fprintf(stderr, "usage: %s [FILE_COUNT]\n", argv[0]);
      return EXIT_FAILURE;
    }
    files = (unsigned)value;
  }

  struct timing plain = run_case("ext2", false, files);
  struct timing dynamic = run_case("ext2+", true, files);
  printf("ratio    create+write=%.2fx  lookup=%.2fx  unlink=%.2fx\n", (double)dynamic.create_ns / plain.create_ns,
         (double)dynamic.lookup_ns / plain.lookup_ns, (double)dynamic.unlink_ns / plain.unlink_ns);
  return EXIT_SUCCESS;
}
