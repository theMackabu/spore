#define _GNU_SOURCE
#include <arm_neon.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint8_t src[1 << 20], dst[1 << 20];

static uint64_t now(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) _Exit(2);
  return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void sample(const char *name, uint64_t start, uint64_t check) {
  uint64_t ns = now() - start;
  printf("SAMPLE %s %" PRIu64 " %" PRIu64 "\n", name, ns, check);
}

int main(void) {
  if (write(1, "SPORE_BENCH_ENTRY\n", 18) != 18) return 2;
  setvbuf(stdout, NULL, _IONBF, 0);
  uint64_t start = now(), a = 1, b = 0, remaining = 1u << 22;
  __asm__ volatile("1: add %0,%0,#7\n eor %1,%1,%0\n subs %2,%2,#1\n b.ne 1b"
                   : "+&r"(a), "+&r"(b), "+&r"(remaining)::"cc");
  sample("integer", start, a ^ b);

  uint64_t check = 0;
  start = now();
  for (int i = 0; i < 100000; ++i)
    check += syscall(SYS_getpid);
  sample("getpid", start, check);

  check = 0;
  start = now();
  for (int i = 0; i < 100000; ++i) {
    register long nr __asm__("x8") = SYS_getpid;
    register long result __asm__("x0");
    __asm__ volatile("svc 0" : "=r"(result) : "r"(nr) : "cc", "memory");
    check += (uint64_t)result;
  }
  sample("getpid_raw", start, check);

  struct statfs fs;
  if (statfs("/tmp", &fs)) return 6;
  if (fs.f_type != 0x01021994 && mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777")) return 6;
  if (statfs("/tmp", &fs) || fs.f_type != 0x01021994) return 6;
  int fd = open("/tmp/bench", O_CREAT | O_RDWR | O_TRUNC, 0600);
  uint8_t buf[64];
  memset(buf, 0x5a, sizeof(buf));
  if (fd < 0 || write(fd, buf, sizeof(buf)) != sizeof(buf)) return 3;
  check = 0;
  start = now();
  for (int i = 0; i < 100000; ++i) {
    if (pread(fd, buf, sizeof(buf), 0) != sizeof(buf)) return 4;
    check += buf[i & 63];
  }
  sample("pread64", start, check);
  close(fd);

  for (size_t i = 0; i < sizeof(src); ++i)
    src[i] = (uint8_t)(i ^ (i >> 8));
  memset(dst, 1, sizeof(dst));
  check = 0;
  start = now();
  for (unsigned i = 0; i < 128; ++i) {
    src[i * 4093] ^= (uint8_t)i;
    memcpy(dst, src, sizeof(src));
    __asm__ volatile("" ::"r"(dst) : "memory");
    check += dst[i * 4093];
  }
  sample("copy128m", start, check);

  uint32x4_t x = {1, 2, 3, 4}, y = {5, 6, 7, 8}, k = {11, 13, 17, 19};
  start = now();
  for (int i = 0; i < 200000; ++i) {
    uint32x4_t prev = x;
    x = vsha256hq_u32(x, y, k);
    y = vsha256h2q_u32(y, prev, k);
  }
  sample("sha256_rounds", start, x[0] ^ y[0]);

  check = 0;
  start = now();
  for (int i = 0; i < 200; ++i) {
    pid_t child = fork();
    if (child == 0) _exit(42);
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) return 5;
    check += WEXITSTATUS(status);
  }
  sample("fork_wait", start, check);
  start = now();
  FILE *pipeline = popen("/bin/busybox seq 1 25000 | /bin/busybox sort -r | /bin/busybox sha256sum", "r");
  char sum[80];
  if (!pipeline || !fgets(sum, sizeof(sum), pipeline) || pclose(pipeline) != 0 || strlen(sum) < 64) return 7;
  sum[64] = 0;
  sample("sort_pipe", start, 25000);
  printf("SORT_HASH %s\n", sum);
  puts("SPORE_BENCH_DONE");
  syscall(0x4006, 0);
  reboot(RB_POWER_OFF);
  return 0;
}
