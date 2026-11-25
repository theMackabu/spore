#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

struct mapping {
  unsigned char *data;
  size_t size;
  unsigned char tag;
};

static uint32_t random_word(uint32_t *seed) {
  *seed ^= *seed << 13;
  *seed ^= *seed >> 17;
  *seed ^= *seed << 5;
  return *seed;
}

static long free_kib(void) {
  FILE *info = fopen("/proc/meminfo", "r");
  long kib;
  assert(info && fscanf(info, "MemTotal: %*d kB MemFree: %ld", &kib) == 1);
  assert(fclose(info) == 0);
  return kib;
}

static void release(struct mapping *m) {
  if (!m->data) { return; }
  for (size_t i = 0; i < m->size; i += 4096) {
    assert(m->data[i] == (unsigned char)(m->tag + i / 4096));
  }
  assert(munmap(m->data, m->size) == 0);
  *m = (struct mapping){0};
}

static void io_roundtrip(uint32_t *seed) {
  unsigned char original[4096], copy[sizeof(original)];
  size_t n = 1 + random_word(seed) % sizeof(original);
  for (size_t i = 0; i < n; ++i) {
    original[i] = (unsigned char)random_word(seed);
  }
  if (random_word(seed) & 1) {
    int ends[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == 0);
    assert(write(ends[0], original, n) == (ssize_t)n && close(ends[0]) == 0);
    for (size_t pos = 0; pos < n;) {
      ssize_t got = read(ends[1], copy + pos, 1 + random_word(seed) % (n - pos));
      assert(got > 0);
      pos += (size_t)got;
    }
    assert(memcmp(original, copy, n) == 0 && read(ends[1], copy, 1) == 0 && close(ends[1]) == 0);
    return;
  }
  int fd = open("/tmp/stress", O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0 && write(fd, original, n) == (ssize_t)n);
  assert(unlink("/tmp/stress") == 0 && pread(fd, copy, n, 0) == (ssize_t)n);
  assert(memcmp(original, copy, n) == 0 && close(fd) == 0);
}

static void batch(uint32_t *seed) {
  struct mapping slots[24] = {0};
  for (unsigned turn = 0; turn < 256; ++turn) {
    struct mapping *m = &slots[random_word(seed) % 24];
    if (m->data) {
      release(m);
    } else {
      m->size = 4096 * (1 + random_word(seed) % 48);
      m->tag = (unsigned char)random_word(seed);
      m->data = mmap(NULL, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      assert(m->data != MAP_FAILED);
      for (size_t i = 0; i < m->size; i += 4096) {
        assert(m->data[i] == 0);
        m->data[i] = (unsigned char)(m->tag + i / 4096);
      }
    }
    if (random_word(seed) % 8 == 0) { io_roundtrip(seed); }
  }
  for (unsigned i = 0; i < 24; ++i) {
    release(&slots[i]);
  }
}

int main(int argc, char **argv) {
  assert(argc == 2);
  uint32_t seed = strtoul(argv[1], NULL, 10);
  assert(seed);
  batch(&seed);
  long start = free_kib();
  for (unsigned i = 0; i < 16; ++i) {
    batch(&seed);
  }
  long loss = start - free_kib();
  printf("seed=%s retained_kib=%ld\n", argv[1], loss);
  return loss > 256;
}
