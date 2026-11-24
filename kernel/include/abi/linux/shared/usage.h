#pragma once
#include <stdint.h>

struct domain;
struct resource_usage;
int linux_write_rusage(struct domain *domain, uint64_t addr, const struct resource_usage *usage);
