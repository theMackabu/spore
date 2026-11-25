#include <stdint.h>
#include <string.h>

static uint8_t src[65536 + 64], dst[65536 + 64];

int main(void) {
  for (unsigned i = 0; i < sizeof(src); i++)
    src[i] = (uint8_t)(i * 7 + (i >> 7));
  for (unsigned i = 1; i <= 32; i++) {
    memset(dst, 0xa5, sizeof(dst));
    unsigned length = 65536 - i, offset = 33 - i;
    memcpy(dst + i, src + offset, length);
    if (memcmp(src + offset, dst + i, length) || dst[i - 1] != 0xa5 || dst[i + length] != 0xa5) return 1;
  }
  return 42;
}
