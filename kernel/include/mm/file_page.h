#pragma once

#include <stdbool.h>
#include <stdint.h>

struct vmm_file_writer;
struct vmm_file_page {
  uint64_t pa;
  uint64_t *writer_pte;
  uint64_t writer_va;
  struct vmm_file_page *next;
  struct vmm_file_writer *writers;
};

void vmm_file_pages_init(void (*mark_dirty)(struct vmm_file_page *page));
void vmm_file_page_register(struct vmm_file_page *page);
void vmm_file_page_unregister(struct vmm_file_page *page);
bool vmm_file_writer_add(uint64_t pa, uint64_t *pte, uint64_t va);
void vmm_file_writer_remove(uint64_t pa, uint64_t *pte);
void vmm_file_page_rearm(struct vmm_file_page *page);
void vmm_write_protect_file_pte(uint64_t *pte, uint64_t va);
