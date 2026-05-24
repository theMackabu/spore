#include <spore.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *type_name(mode_t mode) {
  if (S_ISREG(mode)) { return "regular file"; }
  if (S_ISDIR(mode)) { return "directory"; }
  if (S_ISCHR(mode)) { return "character special file"; }
  return "special file";
}

int main(int argc, char **argv) {
  if (argc < 2) { return usage("stat", "PATH..."); }
  int rc = EXIT_SUCCESS;
  for (int i = 1; i < argc; ++i) {
    struct stat st;
    if (stat(argv[i], &st) != 0) {
      perror(argv[i]);
      rc = EXIT_FAILURE;
      continue;
    }
    printf("  File: %s\n", argv[i]);
    printf("  Size: %lld\tBlocks: %lld\tIO Block: %ld\t%s\n", (long long)st.st_size, (long long)st.st_blocks,
           (long)st.st_blksize, type_name(st.st_mode));
    printf("Device: %llu\tInode: %llu\tLinks: %lu\n", (unsigned long long)st.st_dev, (unsigned long long)st.st_ino,
           (unsigned long)st.st_nlink);
    printf("Access: (%04o)\tUid: (%u)\tGid: (%u)\n", (unsigned)(st.st_mode & 07777), (unsigned)st.st_uid,
           (unsigned)st.st_gid);
  }
  return rc;
}
