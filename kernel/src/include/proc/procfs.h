#pragma once

#include "cell.h"

int64_t cell_procfs_read_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len);
