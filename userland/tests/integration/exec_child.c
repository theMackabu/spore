#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static int exec_private_data = 0x12345678;

int main(int argc, char **argv) {
  if (syscall(SYS_gettid) != getpid()) { return 43; }
  if (argc == 4 && strcmp(argv[1], "check-cmdline") == 0) {
    argv[0][0] = 'C';
    const char expected[] = "Child\0check-cmdline\0two words\0";
    int fd = open("/proc/self/cmdline", O_RDONLY);
    char bytes[sizeof(expected) + 1];
    size_t got = 0;
    while (fd >= 0 && got < sizeof(bytes)) {
      ssize_t n = read(fd, bytes + got, 1);
      if (n <= 0) { break; }
      ++got;
    }
    if (fd >= 0) { close(fd); }
    return got == sizeof(expected) && memcmp(bytes, expected, sizeof(expected)) == 0 ? 0 : 43;
  }
  const char *arg = argc > 1 ? argv[1] : "missing";
  if (strcmp(arg, "parent-death-signal") == 0) {
    int value;
    return !prctl(PR_GET_PDEATHSIG, &value) && value == SIGUSR1 && !prctl(PR_SET_PDEATHSIG, 0) &&
               !prctl(PR_GET_PDEATHSIG, &value) && value == 0
             ? 0
             : 43;
  }
  if (strcmp(arg, "mapped-user") == 0) {
    return !getuid() && !getgid() && !getauxval(AT_EUID) && !getauxval(AT_EGID) ? 0 : 43;
  }
  if (strcmp(arg, "cap-bound") == 0) {
    struct __user_cap_header_struct hdr = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[2];
    return !syscall(SYS_capget, &hdr, caps) && !(caps[0].permitted & (1u << CAP_SYS_ADMIN)) ? 0 : 43;
  }
  if (strcmp(arg, "filtered") == 0) {
    return prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1 && syscall(SYS_getppid) == -1 && errno == EACCES ? 0 : 43;
  }
  if (strcmp(arg, "poison-data") == 0) {
    exec_private_data = 0x5a5a5a5a;
    printf("[spore] exec child: poisoned private data\n");
    return 0;
  }
  if (strcmp(arg, "check-data") == 0) {
    int ok = exec_private_data == 0x12345678;
    printf("[spore] exec child: private data %s value=0x%x\n", ok ? "clean" : "dirty", exec_private_data);
    return ok ? 0 : 43;
  }
  extern char _end[];
  uintptr_t end = (uintptr_t)_end;
  uintptr_t heap = (uintptr_t)sbrk(0);
  uintptr_t loader = getauxval(AT_BASE);
  if (heap < end || (loader > end && heap >= loader)) {
    printf("[spore] exec heap outside executable range\n");
    return 43;
  }
  printf("[spore] exec child: argv=%s\n", arg);
  return 42;
}
