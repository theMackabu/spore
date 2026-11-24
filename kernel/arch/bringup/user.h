#pragma once

#include "init/boot_info.h"
#include "mm/pmm.h"

constexpr uint64_t USER_TOP = 1ull << 32;
enum page_flags { PAGE_READ = 1, PAGE_WRITE = 2, PAGE_EXEC = 4, PAGE_USER = 8 };

uint64_t active_root(void);
void install_root(uint64_t root_pa);
uint64_t table_entry(uint64_t pa);
uint64_t entry_phys(uint64_t entry);
uint64_t page_entry(uint64_t pa, unsigned flags);
bool page_allows(uint64_t entry, unsigned flags);
unsigned page_levels(void);
bool map_page(uint64_t va, uint64_t pa, unsigned flags);
bool status_init(uint64_t hhdm);
void status_write(const uint8_t *bytes, size_t n);

[[noreturn]] void user_enter(uint64_t pc, uint64_t sp);
[[noreturn]] void user_start(const struct spore_boot_info *boot);
[[noreturn]] void user_finish(char kind, unsigned value);
int64_t user_syscall(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
