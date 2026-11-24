#pragma once

#include <stdint.h>

void aarch64_publish_table(uint64_t *slot, uint64_t descriptor);
void aarch64_tlb_flush_all(void);
void aarch64_tlb_flush_pages(uint64_t first_va, uint64_t mask);
void aarch64_tlb_flush_va(uint64_t va);
void aarch64_enable_ttbr0(void);
