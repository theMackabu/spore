#include "mm/pool.h"
#include "net/unix.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/pipe.h"

struct unix_rights {
  struct unix_rights *next;
  uint64_t start, end;
  unsigned count, pool;
  struct open_file *files[];
};

static const unsigned capacities[] = {4, 16, 64, UNIX_MAX_RIGHTS};
static struct object_pool pools[sizeof(capacities) / sizeof(*capacities)];

static struct open_file *sockets;
static bool collection_pending, collecting;
static size_t queued_sockets;

static bool is_unix(const struct open_file *file) {
  return file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER;
}

void unix_track_socket(struct open_file *file) {
  file->unix_gc_next = sockets;
  file->unix_gc_link = &sockets;
  if (sockets) { sockets->unix_gc_link = &file->unix_gc_next; }
  sockets = file;
}

void unix_untrack_socket(struct open_file *file) {
  *file->unix_gc_link = file->unix_gc_next;
  if (file->unix_gc_next) { file->unix_gc_next->unix_gc_link = file->unix_gc_link; }
}

void unix_check_last_reference(struct open_file *file) {
  if (file->unix_inflight && file->refcount == file->unix_inflight + file->unix_pending_owned) {
    collection_pending = true;
  }
}

int unix_prepare_rights(struct domain *domain, const int32_t *fds, unsigned count, struct unix_rights **out) {
  *out = NULL;
  if (!count) { return 0; }
  if (count > UNIX_MAX_RIGHTS) { return -22; }
  for (unsigned i = 0; i < count; ++i) {
    struct open_file *file = cell_file_at(domain, fds[i]);
    if (!file) { return -9; }
    if (file->type == OPEN_URING) { return -22; }
  }
  unsigned index = 0;
  while (count > capacities[index]) {
    ++index;
  }
  struct object_pool *pool = &pools[index];
  size_t size = sizeof(struct unix_rights) + capacities[index] * sizeof(struct open_file *);
  if (!pool->limit && !pool_init(pool, size, SIZE_MAX)) { return -12; }
  struct unix_rights *rights = pool_alloc(pool);
  if (!rights) { return -12; }
  rights->count = count;
  rights->pool = index;
  for (unsigned i = 0; i < count; ++i) {
    rights->files[i] = cell_file_at(domain, fds[i]);
    cell_retain_open_file(rights->files[i]);
  }
  *out = rights;
  return 0;
}

void unix_release_rights(struct unix_rights *rights) {
  if (!rights) { return; }
  for (unsigned i = 0; i < rights->count; ++i) {
    cell_release_open_file(rights->files[i]);
  }
  pool_free(&pools[rights->pool], rights);
}

void unix_queue_rights(struct unix_rights_queue *queue, struct unix_rights *rights, uint64_t start, uint64_t end) {
  for (unsigned i = 0; i < rights->count; ++i) {
    if (is_unix(rights->files[i])) {
      ++rights->files[i]->unix_inflight;
      ++queued_sockets;
      unix_check_last_reference(rights->files[i]);
    }
  }
  rights->start = start;
  rights->end = end;
  if (queue->tail) {
    queue->tail->next = rights;
  } else {
    queue->head = rights;
  }
  queue->tail = rights;
}

static void drop_head(struct unix_rights_queue *queue) {
  struct unix_rights *rights = queue->head;
  queue->head = rights->next;
  if (!queue->head) { queue->tail = NULL; }
  for (unsigned i = 0; i < rights->count; ++i) {
    if (is_unix(rights->files[i])) {
      --rights->files[i]->unix_inflight;
      --queued_sockets;
    }
  }
  unix_release_rights(rights);
}

void unix_discard_rights(struct unix_rights_queue *queue, uint64_t consumed) {
  while (queue->head && queue->head->start < consumed) {
    drop_head(queue);
  }
}

void unix_clear_rights(struct unix_rights_queue *queue) {
  while (queue->head) {
    drop_head(queue);
  }
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
    cell_retain_open_file(rights->files[count]);
    cell_assign_fd(domain, fd, rights->files[count], cloexec);
    fds[count++] = fd;
    start = fd + 1;
  }
  *truncated = count < rights->count;
  return count;
}

static void mark(struct open_file *file, struct open_file **work) {
  if (!is_unix(file) || file->unix_reachable) { return; }
  file->unix_reachable = true;
  file->unix_gc_work = *work;
  *work = file;
}

bool unix_collect_cycles(void) {
  if (!collection_pending || !queued_sockets || collecting) { return false; }
  collecting = true;
  collection_pending = false;
  struct open_file *work = NULL;
  for (struct open_file *file = sockets; file; file = file->unix_gc_next) {
    file->unix_reachable = false;
    if (file->refcount > file->unix_inflight + file->unix_pending_owned) { mark(file, &work); }
  }
  while (work) {
    struct open_file *file = work;
    work = file->unix_gc_work;
    for (struct open_file *child = file->unix_pending_head; child; child = child->unix_pending_next) {
      mark(child, &work);
    }
    if (!file->unix_connected) { continue; }
    for (struct unix_rights *r = cell_pipe_rights(file->unix_rx_pipe)->head; r; r = r->next) {
      for (unsigned i = 0; i < r->count; ++i) {
        mark(r->files[i], &work);
      }
    }
  }
  for (struct open_file *file = sockets; file; file = file->unix_gc_next) {
    if (!file->unix_reachable) {
      cell_retain_open_file(file);
      file->unix_gc_work = work;
      work = file;
    }
  }
  bool reclaimed = work != NULL;
  while (work) {
    struct open_file *file = work;
    work = file->unix_gc_work;
    if (file->unix_connected) { unix_clear_rights(cell_pipe_rights(file->unix_rx_pipe)); }
    cell_release_open_file(file);
  }
  collecting = false;
  return reclaimed;
}
