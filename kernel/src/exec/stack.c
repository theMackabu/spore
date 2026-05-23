#include "exec/stack.h"

#include "mem.h"
#include "mm/pmm.h"

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
    AT_CLKTCK = 17,
    AT_SECURE = 23,
    AT_RANDOM = 25,
};

#define STACK_TOP 0x0000fffffff00000ull
#define STACK_SIZE (8 * PAGE_SIZE)

static uint64_t align_down(uint64_t value, uint64_t align) {
    return value & ~(align - 1);
}

static bool write_u64(const struct user_address_space *as, uint64_t va, uint64_t value) {
    return vmm_copy_to_user(as, va, &value, sizeof(value));
}

bool build_initial_stack(struct user_address_space *as,
                         const struct loaded_elf *elf,
                         uint64_t *stack_pointer) {
    for (uint64_t va = STACK_TOP - STACK_SIZE; va < STACK_TOP; va += PAGE_SIZE) {
        if (!vmm_alloc_page(as, va, VMM_USER_READ | VMM_USER_WRITE)) {
            return false;
        }
    }

    const char argv0[] = "/init";
    uint64_t cursor = STACK_TOP;

    cursor -= sizeof(argv0);
    uint64_t argv0_va = cursor;
    if (!vmm_copy_to_user(as, argv0_va, argv0, sizeof(argv0))) {
        return false;
    }

    cursor = align_down(cursor - 16, 16);
    uint64_t random_va = cursor;
    const uint8_t random[16] = {
        0x73, 0x70, 0x6f, 0x72, 0x65, 0x2d, 0x76, 0x30,
        0x2d, 0x72, 0x61, 0x6e, 0x64, 0x6f, 0x6d, 0x21,
    };
    if (!vmm_copy_to_user(as, random_va, random, sizeof(random))) {
        return false;
    }

    const uint64_t aux[][2] = {
        {AT_PHDR, elf->phdr},
        {AT_PHENT, elf->phent},
        {AT_PHNUM, elf->phnum},
        {AT_PAGESZ, PAGE_SIZE},
        {AT_BASE, 0},
        {AT_ENTRY, elf->entry},
        {AT_UID, 0},
        {AT_EUID, 0},
        {AT_GID, 0},
        {AT_EGID, 0},
        {AT_CLKTCK, 100},
        {AT_SECURE, 0},
        {AT_RANDOM, random_va},
        {AT_NULL, 0},
    };

    const uint64_t slots = 1 + 2 + 1 + (sizeof(aux) / sizeof(aux[0])) * 2;
    uint64_t sp = align_down(cursor - slots * sizeof(uint64_t), 16);
    uint64_t p = sp;

    if (!write_u64(as, p, 1)) {
        return false;
    }
    p += 8;
    if (!write_u64(as, p, argv0_va) || !write_u64(as, p + 8, 0)) {
        return false;
    }
    p += 16;
    if (!write_u64(as, p, 0)) {
        return false;
    }
    p += 8;

    for (size_t i = 0; i < sizeof(aux) / sizeof(aux[0]); ++i) {
        if (!write_u64(as, p, aux[i][0]) || !write_u64(as, p + 8, aux[i][1])) {
            return false;
        }
        p += 16;
    }

    *stack_pointer = sp;
    return true;
}
