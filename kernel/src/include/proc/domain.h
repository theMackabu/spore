#pragma once

#include "cell.h"

void cell_domain_reset(void);
struct domain *cell_current_domain_internal(void);
struct domain *cell_domain_slot(size_t index);
size_t cell_domain_index(const struct domain *domain);
uint32_t cell_domain_last_id(void);
struct domain *cell_find_domain(int id);
struct domain *cell_alloc_domain(void);
void cell_copy_domain_metadata(struct domain *dst, const struct domain *src);
void cell_destroy_domain(struct domain *domain);
void cell_set_domain_identity(struct domain *domain, const char *path, const char *const argv[], uint64_t argc);
