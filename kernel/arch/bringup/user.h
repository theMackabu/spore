#pragma once

#include "init/boot_info.h"
#include "mm/pmm.h"

[[noreturn]] void port_monitor(void);
enum page_flags { PAGE_READ = 1, PAGE_WRITE = 2, PAGE_EXEC = 4, PAGE_USER = 8, PAGE_DEVICE = 16 };

struct trap_frame;
extern uint8_t kernel_stack_top[];
void port_prepare_cpus(const struct spore_boot_info *boot);
void platform_cpu_init(uint32_t cpu, uintptr_t stack);
void port_idle_address_space(void);
void port_flush_remote(void);
[[noreturn]] void port_return_to_user(struct trap_frame *frame, uintptr_t stack);
uint64_t active_root(void);
void install_root(uint64_t root_pa);
uint64_t table_entry(uint64_t pa);
uint64_t entry_phys(uint64_t entry);
uint64_t page_entry(uint64_t pa, unsigned flags);
unsigned page_levels(void);
bool map_page(uint64_t va, uint64_t pa, unsigned flags);
bool status_init(uint64_t hhdm);
void status_write(const uint8_t *bytes, size_t n);

[[noreturn]] void user_enter(uint64_t pc, uint64_t sp);
[[noreturn]] void user_start(const struct spore_boot_info *boot);
[[noreturn]] void user_finish(char kind, unsigned value);
