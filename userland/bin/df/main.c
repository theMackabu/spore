#include <spore.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
  long count = syscall(SYS_spore_mountinfo, NULL, 0);
  if (count <= 0) {
    perror("df");
    return EXIT_FAILURE;
  }
  struct mount_info mounts[16];
  if (count > (long)(sizeof(mounts) / sizeof(mounts[0]))) { count = (long)(sizeof(mounts) / sizeof(mounts[0])); }
  long got = syscall(SYS_spore_mountinfo, mounts, (unsigned long)count * sizeof(mounts[0]));
  if (got < 0) {
    perror("df");
    return EXIT_FAILURE;
  }
  if (got < count) { count = got; }
  printf("%-10s %9s %8s %9s %4s %s\n", "Filesystem", "1K-blocks", "Used", "Available", "Use%", "Mounted on");
  for (long i = 0; i < count; ++i) {
    unsigned long long total = (mounts[i].block_count * mounts[i].block_size) / 1024;
    unsigned long long free = (mounts[i].free_blocks * mounts[i].block_size) / 1024;
    unsigned long long used = total > free ? total - free : 0;
    unsigned pct = total == 0 ? 0 : (unsigned)((used * 100 + total - 1) / total);
    printf("%-10s %9llu %8llu %9llu %3u%% %s\n", mounts[i].source, total, used, free, pct, mounts[i].target);
  }
  return EXIT_SUCCESS;
}
