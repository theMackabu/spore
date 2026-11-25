#include <stdint.h>
#include <sys/mman.h>

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
  return munmap(code, 4096) ? 5 : 42;
}
