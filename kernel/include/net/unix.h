#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct domain;
struct open_file;
struct pipe_obj;
struct unix_rights;

constexpr unsigned UNIX_MAX_RIGHTS = 253;

struct unix_rights_queue {
  struct unix_rights *head, *tail;
};

int unix_prepare_rights(struct domain *domain, const int32_t *fds, unsigned count, struct unix_rights **out);
void unix_release_rights(struct unix_rights *rights);
void unix_queue_rights(struct unix_rights_queue *queue, struct unix_rights *rights, uint64_t start, uint64_t end);
void unix_discard_rights(struct unix_rights_queue *queue, uint64_t consumed);
void unix_clear_rights(struct unix_rights_queue *queue);
const struct unix_rights *unix_next_rights(const struct unix_rights_queue *queue, uint64_t offset, uint64_t *bytes);
unsigned unix_install_rights(struct domain *domain, const struct unix_rights *rights, int *fds, unsigned capacity,
                             bool cloexec, bool *truncated);
