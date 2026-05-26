#include "mm/vmm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
  TEST_PAGE_SIZE = 4096,
  TEST_PAGE_COUNT = 64,
};

static uint8_t phys[TEST_PAGE_COUNT * TEST_PAGE_SIZE];
static uint16_t refs[TEST_PAGE_COUNT];
static uint64_t next_pa = TEST_PAGE_SIZE;

static size_t page_index(uint64_t pa) {
  assert((pa % TEST_PAGE_SIZE) == 0);
  size_t idx = (size_t)(pa / TEST_PAGE_SIZE);
  assert(idx < TEST_PAGE_COUNT);
  return idx;
}

uint64_t pmm_alloc_zero_page(void) {
  assert(next_pa + TEST_PAGE_SIZE <= sizeof(phys));
  uint64_t pa = next_pa;
  memset(&phys[pa], 0, TEST_PAGE_SIZE);
  refs[page_index(pa)] = 1;
  next_pa += TEST_PAGE_SIZE;
  return pa;
}

void pmm_free_page(uint64_t pa) {
  size_t idx = page_index(pa);
  assert(refs[idx] > 0);
  --refs[idx];
}

bool pmm_share_page(uint64_t pa) {
  size_t idx = page_index(pa);
  assert(refs[idx] > 0);
  ++refs[idx];
  return true;
}

bool pmm_is_last_ref(uint64_t pa) {
  return refs[page_index(pa)] == 1;
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

  uint64_t cross_a = pmm_alloc_zero_page();
  uint64_t cross_b = pmm_alloc_zero_page();
  assert(vmm_map_page(&as, 0x600000, cross_a, VMM_USER_READ | VMM_USER_WRITE));
  assert(vmm_map_page(&as, 0x601000, cross_b, VMM_USER_READ | VMM_USER_WRITE));
  char cross_in[64];
  for (size_t i = 0; i < sizeof(cross_in); ++i) {
    cross_in[i] = (char)('A' + (i % 26));
  }
  assert(vmm_copy_to_user(&as, 0x600ff0, cross_in, sizeof(cross_in)));
  assert(memcmp(&phys[cross_a + 0xff0], cross_in, 16) == 0);
  assert(memcmp(&phys[cross_b], cross_in + 16, sizeof(cross_in) - 16) == 0);
  char cross_out[sizeof(cross_in)] = {0};
  assert(vmm_copy_from_user(&as, cross_out, 0x600ff0, sizeof(cross_out)));
  assert(memcmp(cross_out, cross_in, sizeof(cross_in)) == 0);

  struct user_address_space child;
  uint64_t cow_pa = pmm_alloc_zero_page();
  memcpy(&phys[cow_pa], "parent", 7);
  assert(vmm_map_page(&as, 0x500000, cow_pa, VMM_USER_READ | VMM_USER_WRITE));
  assert(vmm_clone_cow(&child, &as, 1));
  assert(refs[page_index(cow_pa)] == 2);
  assert(!vmm_user_range_accessible(&as, 0x500000, 1, VMM_ACCESS_WRITE));
  assert(!vmm_user_range_accessible(&child, 0x500000, 1, VMM_ACCESS_WRITE));

  char cow_buf[8] = {0};
  assert(vmm_copy_from_user(&child, cow_buf, 0x500000, 7));
  assert(strcmp(cow_buf, "parent") == 0);

  assert(vmm_handle_cow_fault(&child, 0x500000));
  assert(vmm_user_range_accessible(&child, 0x500000, 1, VMM_ACCESS_WRITE));
  assert(refs[page_index(cow_pa)] == 1);
  assert(vmm_copy_to_user(&child, 0x500000, "child", 6));

  memset(cow_buf, 0, sizeof(cow_buf));
  assert(vmm_copy_from_user(&as, cow_buf, 0x500000, 7));
  assert(strcmp(cow_buf, "parent") == 0);
  uint64_t child_pa = vmm_user_to_phys(&child, 0x500000) & ~(uint64_t)(TEST_PAGE_SIZE - 1);
  assert(child_pa != cow_pa);
  assert(strcmp((char *)&phys[child_pa], "child") == 0);

  assert(vmm_handle_cow_fault(&as, 0x500000));
  assert(vmm_user_range_accessible(&as, 0x500000, 1, VMM_ACCESS_WRITE));
  assert(vmm_copy_to_user(&as, 0x500000, "last", 5));
  assert(strcmp((char *)&phys[cow_pa], "last") == 0);
  return 0;
}
