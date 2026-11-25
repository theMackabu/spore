#include <arm_acle.h>
#include <arm_neon.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>

int main(void) {
  struct termios tty;
  if (tcgetattr(1, &tty)) return 3;
  tty.c_oflag &= ~OPOST;
  if (tcsetattr(1, TCSANOW, &tty)) return 3;
  uint32x4_t a = {0x01234567, 0x23456789, 0x456789ab, 0x6789abcd};
  uint32x4_t b = {0x89abcdef, 0xabcdef01, 0xcdef0123, 0xef012345};
  uint32x4_t c = {0xb72a0404, 0xc0080345, 0xff234a97, 0x78ffe049};
  uint32_t crc = 0x98123456;
  for (unsigned i = 0; i < 16; ++i) {
    uint8x16_t av = vreinterpretq_u8_u32(a), bv = vreinterpretq_u8_u32(b);
    av = vaesmcq_u8(vaeseq_u8(av, bv));
    bv = vaesimcq_u8(vaesdq_u8(bv, av));
    poly128_t p = vmull_p64(vgetq_lane_u64(vreinterpretq_u64_u8(av), 0), vgetq_lane_u64(vreinterpretq_u64_u8(bv), 1));
    if (write(1, &p, sizeof(p)) != sizeof(p)) return 1;
    a = vsha256hq_u32(a, b, c);
    b = vsha256h2q_u32(b, a, c);
    c = vsha256su1q_u32(vsha256su0q_u32(c, a), b, a);
    a = vsha1cq_u32(a, vsha1h_u32(crc), b);
    b = vsha1pq_u32(b, a[3], c);
    c = vsha1mq_u32(c, b[1], a);
    a = vsha1su1q_u32(vsha1su0q_u32(a, b, c), b);
    crc = __crc32cd(__crc32w(crc, c[2]), vgetq_lane_u64(vreinterpretq_u64_u32(a), 1));
    uint32x4_t value = veorq_u32(veorq_u32(a, b), c);
    if (write(1, &value, sizeof(value)) != sizeof(value) || write(1, &crc, sizeof(crc)) != sizeof(crc)) return 2;
  }
  return 42;
}
