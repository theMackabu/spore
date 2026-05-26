#include "exec/stack.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
  PAGE_SIZE = 4096,
  TEST_STACK_SIZE = USER_STACK_SIZE,
};

#define TEST_STACK_TOP USER_STACK_TOP

static uint8_t stack_mem[TEST_STACK_SIZE];
static bool mapped[TEST_STACK_SIZE / PAGE_SIZE];

static uint64_t stack_offset(uint64_t va) {
  assert(va >= TEST_STACK_TOP - TEST_STACK_SIZE);
  assert(va < TEST_STACK_TOP);
  return va - (TEST_STACK_TOP - TEST_STACK_SIZE);
}

bool vmm_alloc_page(struct user_address_space *as, uint64_t va, uint32_t flags) {
  (void)as;
  assert(flags == (VMM_USER_READ | VMM_USER_WRITE));
  assert((va % PAGE_SIZE) == 0);
  mapped[stack_offset(va) / PAGE_SIZE] = true;
  return true;
}

bool vmm_is_mapped(const struct user_address_space *as, uint64_t va) {
  (void)as;
  assert((va % PAGE_SIZE) == 0);
  return mapped[stack_offset(va) / PAGE_SIZE];
}

bool vmm_copy_to_user(const struct user_address_space *as, uint64_t dst, const void *src, size_t len) {
  (void)as;
  uint64_t off = stack_offset(dst);
  assert(off + len <= sizeof(stack_mem));
  memcpy(&stack_mem[off], src, len);
  return true;
}

static uint64_t read_u64(uint64_t va) {
  uint64_t value;
  memcpy(&value, &stack_mem[stack_offset(va)], sizeof(value));
  return value;
}

int main(void) {
  struct user_address_space as = {0};
  struct loaded_elf elf = {
    .entry = 0x10255d0,
    .phdr = 0x1000040,
    .phent = 56,
    .phnum = 9,
  };
  const char *argv[] = {"/bin/msh"};
  const struct exec_stack_credentials creds = {
    .uid = 1000,
    .euid = 0,
    .gid = 1000,
    .egid = 20,
  };
  uint64_t sp = 0;

  assert(build_initial_stack_args(&as, &elf, argv, 1, NULL, 0, &creds, &sp));
  assert((sp % 16) == 0);
  size_t mapped_count = 0;
  for (size_t i = 0; i < sizeof(mapped) / sizeof(mapped[0]); ++i) {
    if (mapped[i]) { ++mapped_count; }
  }
  assert(mapped_count < sizeof(mapped) / sizeof(mapped[0]));
  assert(mapped[(USER_STACK_SIZE / PAGE_SIZE) - 1]);

  assert(read_u64(sp) == 1);
  uint64_t argv0 = read_u64(sp + 8);
  assert(read_u64(sp + 16) == 0);
  assert(strcmp((const char *)&stack_mem[stack_offset(argv0)], "/bin/msh") == 0);
  assert(read_u64(sp + 24) == 0);

  bool saw_phdr = false;
  bool saw_entry = false;
  bool saw_random = false;
  bool saw_platform = false;
  bool saw_hwcap = false;
  bool saw_execfn = false;
  bool saw_uid = false;
  bool saw_euid = false;
  bool saw_gid = false;
  bool saw_egid = false;
  for (uint64_t p = sp + 32;; p += 16) {
    uint64_t key = read_u64(p);
    uint64_t value = read_u64(p + 8);
    if (key == 0) { break; }
    if (key == 3 && value == elf.phdr) { saw_phdr = true; }
    if (key == 9 && value == elf.entry) { saw_entry = true; }
    if (key == 25 && value >= TEST_STACK_TOP - TEST_STACK_SIZE && value < TEST_STACK_TOP) { saw_random = true; }
    if (key == 15 && strcmp((const char *)&stack_mem[stack_offset(value)], "aarch64") == 0) { saw_platform = true; }
    if (key == 16 && (value & 0x3) == 0x3) { saw_hwcap = true; }
    if (key == 31 && value == argv0) { saw_execfn = true; }
    if (key == 11 && value == creds.uid) { saw_uid = true; }
    if (key == 12 && value == creds.euid) { saw_euid = true; }
    if (key == 13 && value == creds.gid) { saw_gid = true; }
    if (key == 14 && value == creds.egid) { saw_egid = true; }
  }
  assert(saw_phdr);
  assert(saw_entry);
  assert(saw_random);
  assert(saw_platform);
  assert(saw_hwcap);
  assert(saw_execfn);
  assert(saw_uid);
  assert(saw_euid);
  assert(saw_gid);
  assert(saw_egid);
  return 0;
}
