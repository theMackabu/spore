#pragma once

#include "cell.h"

void cell_futex_cleanup_robust_list(struct thread *thread);
int cell_futex_wake_domain(struct domain *domain, uint64_t uaddr, uint32_t count);
