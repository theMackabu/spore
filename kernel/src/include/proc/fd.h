#pragma once

#include "cell.h"

void cell_fd_table_reset(void);
int cell_find_free_fd(struct domain *domain, int start);
struct open_file *cell_alloc_open_file(void);
void cell_retain_open_file(struct open_file *file);
void cell_release_open_file(struct open_file *file);
void cell_close_all_fds(struct domain *domain);
bool cell_init_stdio(struct domain *domain);
void cell_copy_fd_table(struct domain *dst, const struct domain *src);
void cell_copy_open_path(struct open_file *file, const char *path);
int64_t cell_eventfd_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
int64_t cell_eventfd_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
