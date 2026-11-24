#pragma once

#include <stdbool.h>
#include <stdint.h>

struct domain;

void cell_accounting_reset(void);
uint64_t cell_account_tick(uint32_t cpu, bool from_lower_el, bool idle);
bool cell_charge_cpu_budget(struct domain *domain, bool from_lower_el);
