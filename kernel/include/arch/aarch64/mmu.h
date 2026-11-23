#pragma once

#include <stdint.h>

void aarch64_tlb_flush_all(void);
void aarch64_tlb_flush_va(uint64_t va);
void aarch64_enable_ttbr0(void);
