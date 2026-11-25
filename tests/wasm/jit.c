#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static int shared_code(void) {
  char path[] = "/tmp/jit-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0 || unlink(path) || ftruncate(fd, 8192)) { return 6; }
  uint32_t *write = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  uint32_t *exec = mmap(NULL, 8192, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
  close(fd);
  if (write == MAP_FAILED || exec == MAP_FAILED) { return 7; }
  write[1023] = 0x14000001;
  write[1025] = 0xd65f03c0;
  const volatile uint32_t *view = exec;
  if (view[1023] != write[1023] || view[1025] != write[1025]) { return 10; }
  unsigned (*call)(void) = (void *)(exec + 1023);
  for (unsigned value = 1; value <= 3; ++value) {
    write[1024] = 0x52800000 | (value << 5);
    __builtin___clear_cache((char *)(write + 1023), (char *)(write + 1026));
    __builtin___clear_cache((char *)(exec + 1023), (char *)(exec + 1026));
    for (unsigned i = 0; i < 4096; ++i) {
      if (call() != value) { return 8; }
    }
  }
  return munmap(write, 8192) || munmap(exec, 8192) ? 9 : 42;
}

int main(void) {
  uint32_t *code = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED) { return 1; }
  code[0] = 0x52800001;
  code[2] = 0x71000400;
  code[3] = 0x54ffffc1;
  code[4] = 0x2a0103e0;
  code[5] = 0xd65f03c0;
  for (unsigned i = 1; i <= 42; ++i) {
    code[1] = 0x11000021 | (i << 10);
    __builtin___clear_cache((char *)code, (char *)(code + 6));
    if (mprotect(code, 4096, PROT_READ | PROT_EXEC)) { return 2; }
    unsigned (*generated)(unsigned) = (void *)code;
    if (generated(2048) != 2048 * i) { return 3; }
    if (mprotect(code, 4096, PROT_READ | PROT_WRITE)) { return 4; }
  }
  return munmap(code, 4096) ? 5 : shared_code();
}
