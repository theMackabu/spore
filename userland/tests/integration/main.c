#include "integration.h"

#include "dirent64.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <unistd.h>

constexpr int SYS_SPORE_RESIDENT = 0x4003;

static uintptr_t integration_page_down(uintptr_t value) {
  return value & ~(uintptr_t)4095;
}

int main(void) {
  if ((fcntl(STDOUT_FILENO, F_GETFL) & O_ACCMODE) != O_WRONLY ||
      (fcntl(STDERR_FILENO, F_GETFL) & O_ACCMODE) != O_WRONLY) {
    printf("[spore] console output access mode: FAIL\n");
    return 1;
  }
  size_t len = 8 * 1024 * 1024;
  unsigned char *mem = malloc(len);
  if (mem == NULL) {
    printf("[spore] cell 1: malloc 8MB ... failed\n");
    return 1;
  }
  uintptr_t base = integration_page_down((uintptr_t)mem);
  size_t span = len + ((uintptr_t)mem - base);
  long before = integration_raw_syscall3(SYS_SPORE_RESIDENT, (long)base, (long)span, 0);
  for (size_t i = 0; i < len; i += 4096) {
    mem[i] = (unsigned char)(i >> 12);
  }
  long after_touch = integration_raw_syscall3(SYS_SPORE_RESIDENT, (long)base, (long)span, 0);
  printf("[spore] cell 1: malloc 8MB ... resident +%ld pages\n", after_touch - before);
  free(mem);
  long after_free = integration_raw_syscall3(SYS_SPORE_RESIDENT, (long)base, (long)span, 0);
  printf("[spore] cell 1: free      ... resident -%ld pages (returned)\n", after_touch - after_free);

  int fd = openat(AT_FDCWD, "/etc/motd", O_RDONLY);
  if (fd < 0) {
    printf("[spore] cell 1: open /etc/motd failed\n");
    return 1;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    printf("[spore] cell 1: fstat /etc/motd failed\n");
    return 1;
  }
  char buf[64];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  if (n < 0) {
    printf("[spore] cell 1: read /etc/motd failed\n");
    return 1;
  }
  buf[n] = 0;
  if (n > 0 && buf[n - 1] == '\n') { buf[n - 1] = 0; }
  printf("[spore] cell 1: open /etc/motd fd=%d size=%ld\n", fd, (long)st.st_size);
  printf("[spore] cell 1: read %ld bytes: \"%s\"\n", (long)n, buf);
  close(fd);

  int dfd = openat(AT_FDCWD, "/", O_RDONLY);
  char dents[256];
  long dent_bytes = syscall(SYS_getdents64, dfd, dents, sizeof(dents));
  printf("[spore] cell 1: listdir / ->");
  for (long off = 0; off < dent_bytes;) {
    struct linux_dirent64 *d = (struct linux_dirent64 *)(dents + off);
    printf(" %s", d->d_name);
    off += d->d_reclen;
  }
  printf("\n");
  close(dfd);

  int ok_all = integration_tmpfile("/tmp");
  ok_all &= integration_tmpfile("/");
  ok_all &= integration_tmpfile("/dev/shm");
  int ok_v3a = integration_thread_demo();
  ok_all = ok_all && ok_v3a;
  int ok_v3b = integration_futex_demo();
  ok_all = ok_all && ok_v3b;
  ok_all = ok_all && integration_pty_demo();
  int ok_v3c = integration_pthread_demo();
  ok_all = ok_all && ok_v3c;
  int ok_v3e = integration_udp_demo();
  ok_all = ok_all && ok_v3e;
  ok_all = ok_all && integration_socket_suite();
  int ok_v3f = integration_egress_demo();
  ok_all = ok_all && ok_v3f;
  ok_all &= integration_seccomp();
  ok_all &= integration_uts_namespace();
  ok_all &= integration_user_namespace();
  ok_all &= integration_capabilities();
  ok_all = ok_all && integration_signal_suite();
  ok_all = ok_all && integration_memory_suite();
  ok_all = ok_all && integration_fork_suite();

  if (after_touch <= before || after_free >= after_touch) { ok_all = 0; }
  if (n <= 0 || strstr(buf, "Spore") == NULL) { ok_all = 0; }
  if (dent_bytes <= 0) { ok_all = 0; }

  int ok_v1 = integration_snapshot_regression();
  printf("[spore] v1 regression (snapshot/spawn/reap): %s\n", ok_v1 ? "PASS" : "FAIL");
  ok_all = ok_all && ok_v1;

  int ok_v2b = integration_timer_budget_demo();
  printf("[spore] v2b timer/budget demo: %s\n", ok_v2b ? "PASS" : "FAIL");
  ok_all = ok_all && ok_v2b;

  int ok_v2c = integration_exec_demo();
  printf("[spore] v2c fork/exec/wait demo: %s\n", ok_v2c ? "PASS" : "FAIL");
  ok_all = ok_all && ok_v2c;
  ok_all = ok_all && integration_exec_private_data();
  ok_all = ok_all && integration_proc_self_links();

  (void)integration_stdin_demo();
  ok_all = ok_all && integration_fs_demo();
  printf("[spore] integration regression: %s\n", ok_all ? "PASS" : "FAIL");
  printf("[spore] cell 1: exit(%d)\n", ok_all ? 0 : 1);
  return ok_all ? 0 : 1;
}
