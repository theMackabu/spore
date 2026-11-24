#pragma once

#include <stddef.h>

void *table_alloc(size_t count, size_t element_size);
void table_free(void *ptr, size_t count, size_t element_size);
