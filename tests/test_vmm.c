#include "mm/vmm.h"
#include "mm/file_page.h"

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

uint64_t pmm_alloc_page(void) {
  if (next_pa + TEST_PAGE_SIZE > sizeof(phys)) { return 0; }
  uint64_t pa = next_pa;
  refs[page_index(pa)] = 1;
  next_pa += TEST_PAGE_SIZE;
  return pa;
}

uint64_t pmm_alloc_zero_page(void) {
  uint64_t pa = pmm_alloc_page();
  if (pa != 0) { memset(&phys[pa], 0, TEST_PAGE_SIZE); }
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

static struct vmm_file_page *dirty_file_page;

static void note_dirty(struct vmm_file_page *page) {
  dirty_file_page = page;
}

static void test_mapped_file_writes(void) {
  uint64_t first_pa = next_pa;
  struct user_address_space as, child, snapshot;
  assert(vmm_user_init(&as, (uintptr_t)phys));
  struct vmm_file_page page = {.pa = pmm_alloc_zero_page()};
  vmm_file_pages_init(note_dirty);
  vmm_file_page_register(&page);
  uint32_t flags = VMM_USER_READ | VMM_USER_WRITE | VMM_MAP_SHARED | VMM_MAP_FILE_CACHE;
  assert(pmm_share_page(page.pa) && vmm_map_page(&as, 0x1000, page.pa, flags));
  assert(pmm_share_page(page.pa) && vmm_map_page(&as, 0x2000, page.pa, flags));
  assert(vmm_user_buffer_accessible(&as, 0x1000, 2 * TEST_PAGE_SIZE, VMM_ACCESS_WRITE));
  assert(!dirty_file_page && !vmm_user_range_accessible(&as, 0x1000, 1, VMM_ACCESS_WRITE));
  assert(vmm_copy_to_user(&as, 0x1000, "A", 1) && dirty_file_page == &page);
  assert(!vmm_direct_user_buffer(&as, 0x1000, 1, VMM_ACCESS_WRITE));
  assert(!vmm_direct_user_buffer(&as, 0x1000, 1, VMM_ACCESS_READ));
  assert(vmm_clone(&child, &as, VMM_FORK) && vmm_clone(&snapshot, &as, VMM_SNAPSHOT));
  assert(vmm_copy_to_user(&as, 0x2000, "B", 1) && vmm_copy_to_user(&child, 0x1000, "C", 1));
  vmm_file_page_rearm(&page);
  dirty_file_page = NULL;
  assert(!vmm_user_range_accessible(&as, 0x1000, 1, VMM_ACCESS_WRITE));
  assert(!vmm_user_range_accessible(&as, 0x2000, 1, VMM_ACCESS_WRITE));
  assert(!vmm_user_range_accessible(&child, 0x1000, 1, VMM_ACCESS_WRITE));
  char value;
  assert(vmm_copy_from_user(&snapshot, &value, 0x1000, 1) && value == 'A');
  assert(vmm_copy_from_user(&as, &value, 0x2000, 1) && value == 'C');
  assert(vmm_copy_to_user(&child, 0x2000, "D", 1) && dirty_file_page == &page);
  assert(vmm_move_range(&child, 0x2000, 0x3000, TEST_PAGE_SIZE));
  assert(vmm_copy_to_user(&child, 0x3000, "E", 1));
  vmm_destroy(&child);
  vmm_file_page_rearm(&page);
  assert(vmm_copy_from_user(&as, &value, 0x1000, 1) && value == 'E');
  vmm_destroy(&as);
  vmm_destroy(&snapshot);
  vmm_file_page_unregister(&page);
  pmm_free_page(page.pa);
  for (uint64_t pa = first_pa; pa < next_pa; pa += TEST_PAGE_SIZE) { assert(refs[page_index(pa)] == 0); }
  next_pa = first_pa;
  dirty_file_page = NULL;
}

static void test_empty_page_tables(void) {
  uint64_t first = next_pa;
  struct user_address_space as;
  assert(vmm_user_init(&as, (uintptr_t)phys));
  uint16_t empty[TEST_PAGE_COUNT];
  memcpy(empty, refs, sizeof(empty));
  uint64_t va = (1ull << 39) + 4096;
  assert(vmm_alloc_page(&as, va, VMM_USER_READ));
  assert(vmm_alloc_page(&as, va + 4096, VMM_USER_READ));
  vmm_unmap_range(&as, va, va + 4096);
  assert(!vmm_is_mapped(&as, va) && vmm_is_mapped(&as, va + 4096));
  vmm_unmap_range(&as, va + 4096, va + 8192);
  assert(as.resident_pages == 0 && memcmp(refs, empty, sizeof(empty)) == 0);
  next_pa = sizeof(phys) - 3 * TEST_PAGE_SIZE;
  assert(!vmm_alloc_page(&as, 4ull << 39, VMM_USER_READ));
  assert(memcmp(refs, empty, sizeof(empty)) == 0);
  vmm_destroy(&as);
  next_pa = first;
}

int main(void) {
  test_empty_page_tables();
  test_mapped_file_writes();
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

  char unchanged[4] = "abc";
  assert(!vmm_copy_from_user(&as, unchanged, 0x401ffe, sizeof(unchanged)));
  assert(strcmp(unchanged, "abc") == 0);
  assert(!vmm_copy_to_user(&as, 0x401ffe, "xyz", 4));
  assert(phys[rw_pa + TEST_PAGE_SIZE - 2] == 0);

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
  assert(!vmm_direct_user_buffer(&as, 0x400ff0, 32, VMM_ACCESS_READ));
  assert(!vmm_direct_user_buffer(&as, 0x400000, 1, VMM_ACCESS_WRITE));
  assert(!vmm_direct_user_buffer(&as, 0x401ff0, 32, VMM_ACCESS_READ));
  char *span = vmm_direct_user_buffer(&as, 0x600ff0, sizeof(cross_in), VMM_ACCESS_WRITE);
  assert(span && memcmp(span, cross_in, sizeof(cross_in)) == 0);
  span[20] = cross_in[20] = '!';
  char cross_out[sizeof(cross_in)] = {0};
  assert(vmm_copy_from_user(&as, cross_out, 0x600ff0, sizeof(cross_out)));
  assert(memcmp(cross_out, cross_in, sizeof(cross_in)) == 0);

  struct user_address_space child;
  uint64_t cow_pa = pmm_alloc_zero_page();
  memcpy(&phys[cow_pa], "parent", 7);
  assert(vmm_map_page(&as, 0x500000, cow_pa, VMM_USER_READ | VMM_USER_WRITE));
  assert(vmm_alloc_page(&as, 0x700000, VMM_USER_READ | VMM_USER_WRITE | VMM_MAP_SHARED));
  assert(vmm_clone(&child, &as, VMM_FORK));
  assert(vmm_copy_to_user(&child, 0x700000, "shared", 7));
  char shared[7];
  assert(vmm_copy_from_user(&as, shared, 0x700000, sizeof(shared)) && strcmp(shared, "shared") == 0);
  struct user_address_space snapshot;
  assert(vmm_clone(&snapshot, &as, VMM_SNAPSHOT));
  assert(vmm_copy_to_user(&child, 0x700000, "later", 6));
  assert(vmm_copy_from_user(&snapshot, shared, 0x700000, sizeof(shared)) && strcmp(shared, "shared") == 0);
  vmm_destroy(&snapshot);
  vmm_unmap_range(&as, 0x700000, 0x701000);

  vmm_protect_range(&child, 0x400000, 0x401000, VMM_USER_READ | VMM_USER_WRITE);
  assert(vmm_copy_to_user(&child, 0x400000, "new", 4));
  assert(vmm_copy_from_user(&as, buf, 0x400000, sizeof(buf)) && strcmp(buf, "read") == 0);
  assert(refs[page_index(cow_pa)] == 2);
  assert(!vmm_user_range_accessible(&as, 0x500000, 1, VMM_ACCESS_WRITE));
  assert(!vmm_user_range_accessible(&child, 0x500000, 1, VMM_ACCESS_WRITE));

  assert(!vmm_direct_user_buffer(&child, 0x500000, 1, VMM_ACCESS_READ));
  assert(!vmm_direct_user_buffer(&child, 0x500000, 1, VMM_ACCESS_WRITE));
  char cow_buf[8] = {0};
  assert(vmm_copy_from_user(&child, cow_buf, 0x500000, 7));
  assert(strcmp(cow_buf, "parent") == 0);

  vmm_protect_range(&child, 0x500000, 0x501000, 0);
  assert(!vmm_user_range_accessible(&child, 0x500000, 1, VMM_ACCESS_READ));
  assert(!vmm_handle_write_fault(&child, 0x500000));
  vmm_protect_range(&child, 0x500000, 0x501000, VMM_USER_READ | VMM_USER_WRITE);
  assert(vmm_handle_write_fault(&child, 0x500000));
  assert(vmm_user_range_accessible(&child, 0x500000, 1, VMM_ACCESS_WRITE));
  assert(refs[page_index(cow_pa)] == 1);
  assert(vmm_copy_to_user(&child, 0x500000, "child", 6));

  memset(cow_buf, 0, sizeof(cow_buf));
  assert(vmm_copy_from_user(&as, cow_buf, 0x500000, 7));
  assert(strcmp(cow_buf, "parent") == 0);
  uint64_t child_pa = vmm_user_to_phys(&child, 0x500000) & ~(uint64_t)(TEST_PAGE_SIZE - 1);
  assert(child_pa != cow_pa);
  assert(strcmp((char *)&phys[child_pa], "child") == 0);

  assert(vmm_handle_write_fault(&as, 0x500000));
  assert(vmm_user_range_accessible(&as, 0x500000, 1, VMM_ACCESS_WRITE));
  assert(vmm_copy_to_user(&as, 0x500000, "last", 5));
  assert(strcmp((char *)&phys[cow_pa], "last") == 0);
  uint64_t distant = (1ull << 39) + TEST_PAGE_SIZE;
  uint64_t boundary = distant + TEST_PAGE_SIZE;
  assert(vmm_alloc_page(&as, distant, VMM_USER_READ | VMM_USER_WRITE));
  assert(vmm_alloc_page(&as, boundary, VMM_USER_READ | VMM_USER_WRITE));
  assert(vmm_mapped_pages_in_range(&as, 0x601000, boundary) == 2);
  vmm_protect_range(&as, 0x601000, boundary, VMM_USER_READ);
  assert(!vmm_user_range_accessible(&as, distant, 1, VMM_ACCESS_WRITE));
  assert(vmm_user_range_accessible(&as, boundary, 1, VMM_ACCESS_WRITE));
  vmm_unmap_range(&as, 0x601000, boundary);
  assert(vmm_mapped_pages_in_range(&as, 0x600000, boundary + TEST_PAGE_SIZE) == 2);
  assert(vmm_is_mapped(&as, boundary) && !vmm_is_mapped(&as, distant));
  uint64_t dest = (1ull << 30) + 0x1ff000;
  assert(vmm_alloc_page(&as, dest - TEST_PAGE_SIZE, VMM_USER_READ));
  uint64_t available = next_pa;
  next_pa = sizeof(phys);
  assert(!vmm_move_range(&as, 0x400000, dest, 2 * TEST_PAGE_SIZE));
  assert(vmm_user_to_phys(&as, 0x400000) == ro_pa && vmm_user_to_phys(&as, 0x401000) == rw_pa);
  assert(!vmm_is_mapped(&as, dest));
  next_pa = available;
  assert(vmm_move_range(&as, 0x400000, dest, 2 * TEST_PAGE_SIZE));
  assert(vmm_user_to_phys(&as, dest) == ro_pa && vmm_user_to_phys(&as, dest + TEST_PAGE_SIZE) == rw_pa);
  assert(!vmm_is_mapped(&as, 0x400000) && !vmm_is_mapped(&as, 0x401000));
  uint16_t before[TEST_PAGE_COUNT];
  memcpy(before, refs, sizeof(before));
  next_pa = sizeof(phys) - 2 * TEST_PAGE_SIZE;
  struct user_address_space failed;
  assert(!vmm_clone(&failed, &as, VMM_FORK));
  assert(failed.root_pa == 0 && memcmp(refs, before, sizeof(refs)) == 0);
  next_pa = sizeof(phys) - TEST_PAGE_SIZE;
  assert(!vmm_alloc_page(&as, 2ull << 39, VMM_USER_READ));
  assert(!vmm_is_mapped(&as, 2ull << 39));
  assert(refs[TEST_PAGE_COUNT - 1] == 0);
  return 0;
}
