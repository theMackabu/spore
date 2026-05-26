#include "random.h"

#include "mem.h"

#include <stdbool.h>

struct chacha_rng {
  uint32_t key[8];
  uint64_t counter;
  uint32_t nonce[3];
  uint8_t buf[64];
  size_t pos;
  bool seeded;
  bool have_rndr;
};

static struct chacha_rng rng;

/*
 * Kernel CSPRNG for getrandom(2), /dev/{u,}random, and AT_RANDOM.
 *
 * On the QEMU virt target EDK2 exposes FEAT_RNG, so RNDR seeds and
 * periodically refreshes this ChaCha20 stream. If RNDR is unavailable we still
 * mix timer/address jitter, but that fallback is not the intended TLS-grade
 * entropy source.
 */

static uint32_t rotl32(uint32_t v, unsigned n) {
  return (v << n) | (v >> (32u - n));
}

static void quarter_round(uint32_t x[16], unsigned a, unsigned b, unsigned c, unsigned d) {
  x[a] += x[b];
  x[d] = rotl32(x[d] ^ x[a], 16);
  x[c] += x[d];
  x[b] = rotl32(x[b] ^ x[c], 12);
  x[a] += x[b];
  x[d] = rotl32(x[d] ^ x[a], 8);
  x[c] += x[d];
  x[b] = rotl32(x[b] ^ x[c], 7);
}

static uint32_t load32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static bool rndr_supported(void) {
  uint64_t isar0;
  __asm__ volatile("mrs %0, ID_AA64ISAR0_EL1" : "=r"(isar0));
  return ((isar0 >> 60) & 0xfu) != 0;
}

static bool rndr64(uint64_t *out) {
  if (!rng.have_rndr) { return false; }
  uint64_t value;
  uint32_t ok;
  __asm__ volatile("mrs %0, S3_3_C2_C4_0\n\t"
                   "cset %w1, ne"
                   : "=r"(value), "=r"(ok)
                   :
                   : "cc");
  if (ok == 0) { return false; }
  *out = value;
  return true;
}

static uint64_t counter_value(void) {
  uint64_t v;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}

static void chacha_block(uint8_t out[64]) {
  static const uint32_t constants[4] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};
  uint32_t state[16];
  uint32_t work[16];
  state[0] = constants[0];
  state[1] = constants[1];
  state[2] = constants[2];
  state[3] = constants[3];
  for (size_t i = 0; i < 8; ++i) {
    state[4 + i] = rng.key[i];
  }
  state[12] = (uint32_t)rng.counter;
  state[13] = (uint32_t)(rng.counter >> 32);
  state[14] = rng.nonce[0];
  state[15] = rng.nonce[1] ^ rng.nonce[2];
  ++rng.counter;

  for (size_t i = 0; i < 16; ++i) {
    work[i] = state[i];
  }
  for (unsigned i = 0; i < 10; ++i) {
    quarter_round(work, 0, 4, 8, 12);
    quarter_round(work, 1, 5, 9, 13);
    quarter_round(work, 2, 6, 10, 14);
    quarter_round(work, 3, 7, 11, 15);
    quarter_round(work, 0, 5, 10, 15);
    quarter_round(work, 1, 6, 11, 12);
    quarter_round(work, 2, 7, 8, 13);
    quarter_round(work, 3, 4, 9, 14);
  }
  for (size_t i = 0; i < 16; ++i) {
    store32(out + i * 4, work[i] + state[i]);
  }
}

static void absorb_seed(const uint8_t *seed, size_t len) {
  uint8_t block[64];
  chacha_block(block);
  for (size_t i = 0; i < len; ++i) {
    block[i % sizeof(block)] ^= seed[i];
  }
  for (size_t i = 0; i < 8; ++i) {
    rng.key[i] = load32(block + i * 4);
  }
  rng.nonce[0] ^= load32(block + 32);
  rng.nonce[1] ^= load32(block + 36);
  rng.nonce[2] ^= load32(block + 40);
  rng.counter ^= ((uint64_t)load32(block + 44) << 32) | load32(block + 48);
  rng.pos = sizeof(rng.buf);
  kmemset(block, 0, sizeof(block));
}

void random_init(uint64_t seed_hint) {
  rng.have_rndr = rndr_supported();
  rng.key[0] = 0x73706f72u;
  rng.key[1] = 0x652d6373u;
  rng.key[2] = 0x70726e67u;
  rng.key[3] = 0x2d626f6fu;
  rng.key[4] = (uint32_t)seed_hint;
  rng.key[5] = (uint32_t)(seed_hint >> 32);
  rng.key[6] = (uint32_t)(uintptr_t)&rng;
  rng.key[7] = (uint32_t)counter_value();
  rng.counter = counter_value() ^ seed_hint;
  rng.nonce[0] = (uint32_t)(counter_value() >> 8);
  rng.nonce[1] = (uint32_t)(uintptr_t)&random_init;
  rng.nonce[2] = (uint32_t)(uintptr_t)&random_bytes;
  rng.pos = sizeof(rng.buf);
  rng.seeded = true;

  uint64_t seed_words[8];
  for (size_t i = 0; i < sizeof(seed_words) / sizeof(seed_words[0]); ++i) {
    uint64_t word = counter_value() ^ seed_hint ^ ((uint64_t)i << 56);
    uint64_t rndr;
    if (rndr64(&rndr)) { word ^= rndr; }
    seed_words[i] = word;
  }
  absorb_seed((const uint8_t *)seed_words, sizeof(seed_words));
  kmemset(seed_words, 0, sizeof(seed_words));
}

void random_bytes(void *dst, size_t len) {
  if (!rng.seeded) { random_init(counter_value()); }
  uint8_t *out = dst;
  for (size_t i = 0; i < len; ++i) {
    if (rng.pos >= sizeof(rng.buf)) {
      uint64_t rndr;
      if (rndr64(&rndr) && (rng.counter & 0x3fu) == 0) { absorb_seed((const uint8_t *)&rndr, sizeof(rndr)); }
      chacha_block(rng.buf);
      rng.pos = 0;
    }
    out[i] = rng.buf[rng.pos];
    rng.buf[rng.pos++] = 0;
  }
  uint8_t rekey[32];
  chacha_block(rekey);
  for (size_t i = 0; i < 8; ++i) {
    rng.key[i] = load32(rekey + i * 4);
  }
  rng.pos = sizeof(rng.buf);
  kmemset(rekey, 0, sizeof(rekey));
}
