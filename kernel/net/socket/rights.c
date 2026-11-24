#include "net/unix.h"
#include "mm/pool.h"
#include "proc/domain.h"
#include "proc/fd.h"

struct unix_rights {
  struct unix_rights *next;
  uint64_t start, end;
  unsigned count, pool;
  struct open_file *files[];
};

static const unsigned capacities[] = {4, 16, 64, UNIX_MAX_RIGHTS};
static struct object_pool pools[sizeof(capacities) / sizeof(*capacities)];

int unix_prepare_rights(struct domain *domain, const int32_t *fds, unsigned count, struct unix_rights **out) {
  *out = NULL;
  if (!count) { return 0; }
  if (count > UNIX_MAX_RIGHTS) { return -22; }
  for (unsigned i = 0; i < count; ++i) {
    if (fds[i] < 0 || fds[i] >= MAX_FDS || !domain->fds[fds[i]]) { return -9; }
  }
  unsigned index = 0;
  while (count > capacities[index]) { ++index; }
  struct object_pool *pool = &pools[index];
  size_t size = sizeof(struct unix_rights) + capacities[index] * sizeof(struct open_file *);
  if (!pool->limit && !pool_init(pool, size, SIZE_MAX)) { return -12; }
  struct unix_rights *rights = pool_alloc(pool);
  if (!rights) { return -12; }
  rights->count = count;
  rights->pool = index;
  for (unsigned i = 0; i < count; ++i) {
    rights->files[i] = domain->fds[fds[i]];
    cell_retain_open_file(rights->files[i]);
  }
  *out = rights;
  return 0;
}

void unix_release_rights(struct unix_rights *rights) {
  if (!rights) { return; }
  for (unsigned i = 0; i < rights->count; ++i) { cell_release_open_file(rights->files[i]); }
  pool_free(&pools[rights->pool], rights);
}

void unix_queue_rights(struct unix_rights_queue *queue, struct unix_rights *rights, uint64_t start, uint64_t end) {
  rights->start = start;
  rights->end = end;
  if (queue->tail) { queue->tail->next = rights; }
  else { queue->head = rights; }
  queue->tail = rights;
}

static void drop_head(struct unix_rights_queue *queue) {
  struct unix_rights *rights = queue->head;
  queue->head = rights->next;
  if (!queue->head) { queue->tail = NULL; }
  unix_release_rights(rights);
}

void unix_discard_rights(struct unix_rights_queue *queue, uint64_t consumed) {
  while (queue->head && queue->head->start < consumed) { drop_head(queue); }
}

void unix_clear_rights(struct unix_rights_queue *queue) {
  while (queue->head) { drop_head(queue); }
}

const struct unix_rights *unix_next_rights(const struct unix_rights_queue *queue, uint64_t offset, uint64_t *bytes) {
  const struct unix_rights *rights = queue->head;
  if (!rights || rights->start - offset >= *bytes) { return NULL; }
  if (*bytes > rights->end - offset) { *bytes = rights->end - offset; }
  return rights;
}

unsigned unix_install_rights(struct domain *domain, const struct unix_rights *rights, int *fds, unsigned capacity,
                             bool cloexec, bool *truncated) {
  unsigned count = 0;
  int start = 0;
  while (count < capacity && count < rights->count) {
    int fd = cell_find_free_fd(domain, start);
    if (fd < 0) { break; }
    domain->fds[fd] = rights->files[count];
    cell_retain_open_file(domain->fds[fd]);
    domain->fd_flags[fd] = cloexec;
    fds[count++] = fd;
    start = fd + 1;
  }
  *truncated = count < rights->count;
  return count;
}
