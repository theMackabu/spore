#include <stddef.h>
#include <stdint.h>

void random_init(uint64_t seed_hint) {
  (void)seed_hint;
}

void random_bytes(void *dst, size_t len) {
  uint8_t *out = dst;
  for (size_t i = 0; i < len; ++i) {
    out[i] = (uint8_t)(0xa5u ^ (uint8_t)i);
  }
}
