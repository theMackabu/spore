#pragma once

#include <stdint.h>

void exceptions_init(void);
[[noreturn]] void enter_el0(uint64_t entry, uint64_t sp, uint64_t arg0, uint64_t lr);
[[noreturn]] void aarch64_call_on_stack(uint64_t kernel_sp, void (*finish)(void *), void *context);
