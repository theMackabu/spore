#include "mm/vmm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    TEST_PAGE_SIZE = 4096,
    TEST_PAGE_COUNT = 16,
};

static uint8_t phys[TEST_PAGE_COUNT * TEST_PAGE_SIZE];
static uint64_t next_pa = TEST_PAGE_SIZE;

uint64_t pmm_alloc_zero_page(void) {
    assert(next_pa < sizeof(phys));
    uint64_t pa = next_pa;
    memset(&phys[pa], 0, TEST_PAGE_SIZE);
    next_pa += TEST_PAGE_SIZE;
    return pa;
}

int main(void) {
    struct user_address_space as;
    assert(vmm_user_init(&as, (uint64_t)(uintptr_t)phys));

    uint64_t ro_pa = pmm_alloc_zero_page();
    memcpy(&phys[ro_pa], "read", 5);
    assert(vmm_map_page(&as, 0x400000, ro_pa, VMM_USER_READ));
    assert(vmm_user_range_accessible(&as, 0x400000, 4, VMM_ACCESS_READ));
    assert(!vmm_user_range_accessible(&as, 0x400000, 4, VMM_ACCESS_WRITE));

    char buf[5] = {0};
    assert(vmm_copy_from_user(&as, buf, 0x400000, 5));
    assert(strcmp(buf, "read") == 0);
    assert(!vmm_copy_to_user(&as, 0x400000, "no", 3));

    uint64_t rw_pa = pmm_alloc_zero_page();
    assert(vmm_map_page(&as, 0x401000, rw_pa, VMM_USER_READ | VMM_USER_WRITE));
    assert(vmm_copy_to_user(&as, 0x401000, "ok", 3));
    assert(strcmp((char *)&phys[rw_pa], "ok") == 0);
    assert(!vmm_user_range_accessible(&as, 0x401ff0, 0x40, VMM_ACCESS_READ));
    return 0;
}

