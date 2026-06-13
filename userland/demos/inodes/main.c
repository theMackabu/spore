#include <spore.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

enum {
  EXT2PLUS_BASE_INO = 0x40000000u,
  DEFAULT_FILES = 270,
  MAX_FILES = 4096,
};

static bool fsinfo(struct fs_info *out) {
  return out != NULL && syscall(SYS_spore_fsinfo, out) == 0;
}

static void print_fsinfo(const char *label, const struct fs_info *info) {
  unsigned long long used = info->inode_count >= info->free_inodes ? info->inode_count - info->free_inodes : 0;
  printf("%-12s total=%llu free=%llu used=%llu\n", label, (unsigned long long)info->inode_count,
         (unsigned long long)info->free_inodes, used);
}

static int parse_count(const char *s) {
  char *end = NULL;
  long value = strtol(s, &end, 10);
  if (s[0] == '\0' || end == NULL || *end != '\0' || value < 1 || value > MAX_FILES) { return -1; }
  return (int)value;
}

static bool write_file(const char *path, int index, unsigned long long fixed_cap) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) {
    perror(path);
    return false;
  }
  char payload[128];
  int len =
    snprintf(payload, sizeof(payload), "ext2+ dynamic inode demo file %d; fixed cap was %llu\n", index, fixed_cap);
  bool ok = len > 0 && write(fd, payload, (size_t)len) == len;
  if (close(fd) != 0) { ok = false; }
  if (!ok) { perror(path); }
  return ok;
}

static bool print_inode(const char *label, const char *path, unsigned long long fixed_cap) {
  struct stat st;
  if (stat(path, &st) != 0) {
    perror(path);
    return false;
  }
  unsigned long long ino = (unsigned long long)st.st_ino;
  printf("%-12s ino=%llu", label, ino);
  if (ino > fixed_cap) { printf("  > fixed cap %llu", fixed_cap); }
  if (ino >= EXT2PLUS_BASE_INO) { printf("  dynamic ext2+"); }
  printf("\n");
  return true;
}

int main(int argc, char **argv) {
  if (argc > 2) { return usage("inodes", "[FILE_COUNT]"); }
  int files = argc == 2 ? parse_count(argv[1]) : DEFAULT_FILES;
  if (files < 0) { return usage("inodes", "[FILE_COUNT]"); }

  struct fs_info before;
  if (!fsinfo(&before)) {
    perror("spore_fsinfo");
    return EXIT_FAILURE;
  }
  unsigned long long fixed_cap = (unsigned long long)before.inode_count;

  char dir[128];
  snprintf(dir, sizeof(dir), "/home/spore/demos/inodes.%ld", (long)getpid());
  if (mkdir(dir, 0755) != 0) {
    perror(dir);
    return EXIT_FAILURE;
  }

  char first[160] = "";
  char transition[160] = "";
  char last[160] = "";
  for (int i = 0; i < files; ++i) {
    char path[160];
    snprintf(path, sizeof(path), "%s/file-%04d", dir, i);
    if (!write_file(path, i, fixed_cap)) { return EXIT_FAILURE; }
    if (i == 0) { snprintf(first, sizeof(first), "%s", path); }
    if (i == 63) { snprintf(transition, sizeof(transition), "%s", path); }
    if (i == files - 1) { snprintf(last, sizeof(last), "%s", path); }
  }

  struct fs_info after;
  if (!fsinfo(&after)) {
    perror("spore_fsinfo");
    return EXIT_FAILURE;
  }

  printf("ext2+ dynamic inode demo\n");
  printf("created directory: %s\n", dir);
  printf("created files:     %d\n", files);
  printf("dynamic base:      %u\n", EXT2PLUS_BASE_INO);
  print_fsinfo("before", &before);
  print_fsinfo("after", &after);
  printf("capacity delta:    %+lld inodes\n", (long long)(after.inode_count - before.inode_count));
  printf("\n");
  if (!print_inode("directory", dir, fixed_cap)) { return EXIT_FAILURE; }
  if (first[0] != '\0' && !print_inode("first file", first, fixed_cap)) { return EXIT_FAILURE; }
  if (transition[0] != '\0' && !print_inode("file 063", transition, fixed_cap)) { return EXIT_FAILURE; }
  if (last[0] != '\0' && !print_inode("last file", last, fixed_cap)) { return EXIT_FAILURE; }
  return EXIT_SUCCESS;
}
