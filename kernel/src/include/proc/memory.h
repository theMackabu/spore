#pragma once

#include "cell.h"

bool cell_domain_ensure_user_range(struct domain *domain, uint64_t va, size_t len, enum vmm_access access);
