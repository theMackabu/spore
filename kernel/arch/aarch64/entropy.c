#include "arch/aarch64/entropy.h"

bool aarch64_rndr_supported(void) {
  uint64_t isar0;
  __asm__ volatile("mrs %0, ID_AA64ISAR0_EL1" : "=r"(isar0));
  return ((isar0 >> 60) & 0xfu) != 0;
}

bool aarch64_rndr64(uint64_t *out) {
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
