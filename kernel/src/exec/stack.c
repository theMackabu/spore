#include "exec/stack.h"

#include "mem.h"
#include "mm/pmm.h"
#include "random.h"

enum {
  AT_NULL = 0,
  AT_PHDR = 3,
  AT_PHENT = 4,
  AT_PHNUM = 5,
  AT_PAGESZ = 6,
  AT_BASE = 7,
  AT_ENTRY = 9,
  AT_UID = 11,
  AT_EUID = 12,
  AT_GID = 13,
  AT_EGID = 14,
  AT_PLATFORM = 15,
  AT_HWCAP = 16,
  AT_CLKTCK = 17,
  AT_SECURE = 23,
  AT_RANDOM = 25,
  AT_HWCAP2 = 26,
  AT_EXECFN = 31,
  AT_MINSIGSTKSZ = 51,
};

enum {
  AARCH64_HWCAP_FP = 1u << 0,
  AARCH64_HWCAP_ASIMD = 1u << 1,
  AARCH64_HWCAP_AES = 1u << 3,
  AARCH64_HWCAP_PMULL = 1u << 4,
  AARCH64_HWCAP_SHA1 = 1u << 5,
  AARCH64_HWCAP_SHA2 = 1u << 6,
  AARCH64_HWCAP_CRC32 = 1u << 7,
  AARCH64_HWCAP_ATOMICS = 1u << 8,
  SPORE_MINSIGSTKSZ = 8192,
};

static uint64_t align_down(uint64_t value, uint64_t align) {
  return value & ~(align - 1);
}

static bool ensure_stack_range(struct user_address_space *as, uint64_t va, size_t len) {
  if (len == 0 || va < USER_STACK_TOP - USER_STACK_SIZE || va + len < va || va + len > USER_STACK_TOP) { return false; }
  uint64_t page = align_down(va, PAGE_SIZE);
  uint64_t last = align_down(va + len - 1, PAGE_SIZE);
  for (;;) {
    if (!vmm_is_mapped(as, page) && !vmm_alloc_page(as, page, VMM_USER_READ | VMM_USER_WRITE)) { return false; }
    if (page == last) { return true; }
    page += PAGE_SIZE;
  }
}

static bool stack_copy_to_user(struct user_address_space *as, uint64_t va, const void *src, size_t len) {
  return ensure_stack_range(as, va, len) && vmm_copy_to_user(as, va, src, len);
}

static bool stack_write_u64(struct user_address_space *as, uint64_t va, uint64_t value) {
  return stack_copy_to_user(as, va, &value, sizeof(value));
}

static void free_pages(uint64_t pa, uint64_t pages) {
  for (uint64_t i = 0; i < pages; ++i) {
    pmm_free_page(pa + i * PAGE_SIZE);
  }
}

bool build_initial_stack(struct user_address_space *as, const struct loaded_elf *elf, uint64_t *stack_pointer) {
  const char *argv[] = {"/sbin/init"};
  const struct exec_stack_credentials creds = {0};
  return build_initial_stack_args(as, elf, argv, 1, NULL, 0, &creds, stack_pointer);
}

bool build_initial_stack_args(struct user_address_space *as, const struct loaded_elf *elf, const char *const argv[],
                              uint64_t argc, const char *const envp[], uint64_t envc,
                              const struct exec_stack_credentials *creds, uint64_t *stack_pointer) {
  uint64_t cursor = USER_STACK_TOP;
  uint64_t *argv_va = NULL;
  uint64_t *envp_va = NULL;
  uint64_t ptr_pa = 0;
  uint64_t ptr_pages = 0;
  bool ok = false;

  if (creds == NULL) return false;
  if ((argc != 0 && argv == NULL) || (envc != 0 && envp == NULL)) return false;

  uint64_t total_ptrs = argc + envc;
  if (total_ptrs < argc) return false;
  if (total_ptrs != 0) {
    uint64_t ptr_bytes = total_ptrs * sizeof(uint64_t);
    if (ptr_bytes / sizeof(uint64_t) != total_ptrs || ptr_bytes > USER_STACK_SIZE) return false;
    ptr_pages = (ptr_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    ptr_pa = pmm_alloc_contiguous_pages(ptr_pages);
    if (ptr_pa == 0) return false;
    argv_va = (uint64_t *)(uintptr_t)(as->hhdm_offset + ptr_pa);
    envp_va = argv_va + argc;
  }

  for (uint64_t i = 0; i < argc; ++i) {
    size_t len = kstrlen(argv[i]) + 1;
    cursor -= len;
    argv_va[i] = cursor;
    if (!stack_copy_to_user(as, argv_va[i], argv[i], len)) { goto out; }
  }

  for (uint64_t i = 0; i < envc; ++i) {
    size_t len = kstrlen(envp[i]) + 1;
    cursor -= len;
    envp_va[i] = cursor;
    if (!stack_copy_to_user(as, envp_va[i], envp[i], len)) { goto out; }
  }

  static const char platform[] = "aarch64";
  cursor -= sizeof(platform);
  uint64_t platform_va = cursor;
  if (!stack_copy_to_user(as, platform_va, platform, sizeof(platform))) { goto out; }

  cursor = align_down(cursor - 16, 16);
  uint64_t random_va = cursor;
  uint8_t random[16];
  random_bytes(random, sizeof(random));
  if (!stack_copy_to_user(as, random_va, random, sizeof(random))) { goto out; }
  kmemset(random, 0, sizeof(random));

  const uint64_t hwcap = AARCH64_HWCAP_FP | AARCH64_HWCAP_ASIMD | AARCH64_HWCAP_AES | AARCH64_HWCAP_PMULL |
                         AARCH64_HWCAP_SHA1 | AARCH64_HWCAP_SHA2 | AARCH64_HWCAP_CRC32 | AARCH64_HWCAP_ATOMICS;
  const uint64_t aux[][2] = {
    {AT_PHDR, elf->phdr},
    {AT_PHENT, elf->phent},
    {AT_PHNUM, elf->phnum},
    {AT_PAGESZ, PAGE_SIZE},
    {AT_BASE, elf->at_base},
    {AT_ENTRY, elf->entry},
    {AT_UID, creds->uid},
    {AT_EUID, creds->euid},
    {AT_GID, creds->gid},
    {AT_EGID, creds->egid},
    {AT_PLATFORM, platform_va},
    {AT_HWCAP, hwcap},
    {AT_CLKTCK, 100},
    {AT_SECURE, 0},
    {AT_RANDOM, random_va},
    {AT_HWCAP2, 0},
    {AT_EXECFN, argc > 0 ? argv_va[0] : 0},
    {AT_MINSIGSTKSZ, SPORE_MINSIGSTKSZ},
    {AT_NULL, 0},
  };

  const uint64_t slots = 1 + argc + 1 + envc + 1 + (sizeof(aux) / sizeof(aux[0])) * 2;
  uint64_t sp = align_down(cursor - slots * sizeof(uint64_t), 16);
  uint64_t p = sp;

  if (!stack_write_u64(as, p, argc)) { goto out; }
  p += 8;
  for (uint64_t i = 0; i < argc; ++i) {
    if (!stack_write_u64(as, p, argv_va[i])) { goto out; }
    p += 8;
  }
  if (!stack_write_u64(as, p, 0)) { goto out; }
  p += 8;
  for (uint64_t i = 0; i < envc; ++i) {
    if (!stack_write_u64(as, p, envp_va[i])) { goto out; }
    p += 8;
  }
  if (!stack_write_u64(as, p, 0)) { goto out; }
  p += 8;

  for (size_t i = 0; i < sizeof(aux) / sizeof(aux[0]); ++i) {
    if (!stack_write_u64(as, p, aux[i][0]) || !stack_write_u64(as, p + 8, aux[i][1])) { goto out; }
    p += 16;
  }

  *stack_pointer = sp;
  ok = true;
out:
  if (ptr_pa != 0) { free_pages(ptr_pa, ptr_pages); }
  return ok;
}
