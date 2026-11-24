#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "mm/pool.h"

enum { EBADF = 9, ENOMEM = 12, EFAULT = 14, EINVAL = 22, EMFILE = 24, O_CLOEXEC = 02000000 };

static struct object_pool chunks;

static struct fd_chunk *prepare(struct fd_table *table, unsigned index) {
  struct fd_chunk *chunk = fd_chunk_at(table, index);
  if (chunk) { return chunk; }
  if (!chunks.limit && !pool_init(&chunks, sizeof(*chunk), SIZE_MAX)) { return NULL; }
  return table->more[index - 1] = pool_alloc(&chunks);
}

static void trim(struct fd_table *table, unsigned index) {
  struct fd_chunk *chunk = fd_chunk_at(table, index);
  if (index && chunk && !(chunk->occupied | chunk->reserved)) {
    table->more[index - 1] = NULL;
    pool_free(&chunks, chunk);
  }
}

int cell_find_free_fd(struct domain *domain, int start) {
  if (!domain || start < 0) { return -EINVAL; }
  unsigned limit = domain->nofile_soft;
  for (unsigned fd = (unsigned)start; fd < limit;) {
    unsigned index = fd / FDS_PER_CHUNK;
    struct fd_chunk *chunk = fd_chunk_at(&domain->files, index);
    uint32_t free = ~(chunk ? chunk->occupied | chunk->reserved : 0U) & (UINT32_MAX << (fd % FDS_PER_CHUNK));
    if (free) {
      fd = index * FDS_PER_CHUNK + (unsigned)__builtin_ctz(free);
      if (fd >= limit) { break; }
      return chunk || prepare(&domain->files, index) ? (int)fd : -ENOMEM;
    }
    fd = (index + 1) * FDS_PER_CHUNK;
  }
  return -EMFILE;
}

int cell_reserve_fd(struct domain *domain) {
  int fd = cell_find_free_fd(domain, 0);
  if (fd >= 0) { fd_chunk_at(&domain->files, fd / FDS_PER_CHUNK)->reserved |= UINT32_C(1) << (fd % FDS_PER_CHUNK); }
  return fd;
}

void cell_cancel_fd_reservation(struct domain *domain, int fd) {
  unsigned index = (unsigned)fd / FDS_PER_CHUNK;
  struct fd_chunk *chunk = fd_chunk_at(&domain->files, index);
  chunk->reserved &= ~(UINT32_C(1) << (fd % FDS_PER_CHUNK));
  trim(&domain->files, index);
}

int cell_next_open_fd(struct domain *domain, unsigned start) {
  if (start >= MAX_FDS) { return -1; }
  unsigned first = start / FDS_PER_CHUNK;
  uint32_t blocks = domain->files.nonempty & (UINT32_MAX << first);
  while (blocks) {
    unsigned index = (unsigned)__builtin_ctz(blocks);
    struct fd_chunk *chunk = fd_chunk_at(&domain->files, index);
    uint32_t bits = chunk->occupied;
    if (index == first) { bits &= UINT32_MAX << (start % FDS_PER_CHUNK); }
    if (bits) { return (int)(index * FDS_PER_CHUNK + (unsigned)__builtin_ctz(bits)); }
    blocks &= blocks - 1;
  }
  return -1;
}

void cell_assign_fd(struct domain *domain, int fd, struct open_file *file, bool cloexec) {
  unsigned index = (unsigned)fd / FDS_PER_CHUNK, slot = (unsigned)fd % FDS_PER_CHUNK;
  struct fd_table *table = &domain->files;
  struct fd_chunk *chunk = fd_chunk_at(table, index);
  struct open_file *old = chunk->files[slot];
  uint32_t bit = UINT32_C(1) << slot;
  chunk->reserved &= ~bit;
  chunk->files[slot] = file;
  if (file) {
    chunk->occupied |= bit;
    table->nonempty |= UINT32_C(1) << index;
  } else {
    chunk->occupied &= ~bit;
    if (!chunk->occupied) { table->nonempty &= ~(UINT32_C(1) << index); }
  }
  if (cloexec) { chunk->cloexec |= bit; }
  else { chunk->cloexec &= ~bit; }
  if (!file) { trim(table, index); }
  cell_release_open_file(old);
}

int cell_install_fd(struct domain *domain, struct open_file *file, bool cloexec) {
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { cell_release_open_file(file); return fd; }
  cell_assign_fd(domain, fd, file, cloexec);
  return fd;
}

int cell_install_fd_pair(struct domain *domain, struct open_file *a, struct open_file *b, bool cloexec, uint64_t out) {
  int first = cell_find_free_fd(domain, 0);
  int second = first < 0 ? first : cell_find_free_fd(domain, first + 1);
  int fds[] = {first, second};
  if (second < 0 || !vmm_copy_to_user(cell_domain_as(domain), out, fds, sizeof(fds))) {
    if (first >= 0) { trim(&domain->files, (unsigned)first / FDS_PER_CHUNK); }
    if (second >= 0) { trim(&domain->files, (unsigned)second / FDS_PER_CHUNK); }
    cell_release_open_file(a);
    cell_release_open_file(b);
    return second < 0 ? second : -EFAULT;
  }
  cell_assign_fd(domain, first, a, cloexec);
  cell_assign_fd(domain, second, b, cloexec);
  return 0;
}

struct open_file *cell_fd_file(uint64_t fd) { return cell_file_at(cell_current_domain_internal(), fd); }

int cell_close_domain_fd(struct domain *domain, int fd) {
  if (!cell_file_at(domain, fd)) { return -EBADF; }
  cell_assign_fd(domain, fd, NULL, false);
  return 0;
}

int cell_fd_close(int fd) { return cell_close_domain_fd(cell_current_domain_internal(), fd); }

enum fd_range_action { CLOSE_FDS, CLOSE_EXEC_FDS, MARK_CLOEXEC };

static void apply_fd_range(struct domain *domain, unsigned first, unsigned last, enum fd_range_action action) {
  if (first >= MAX_FDS || first > last) { return; }
  if (last >= MAX_FDS) { last = MAX_FDS - 1; }
  unsigned low = first / FDS_PER_CHUNK, high = last / FDS_PER_CHUNK;
  uint32_t blocks = domain->files.nonempty & (UINT32_MAX << low) & (UINT32_MAX >> (31 - high));
  while (blocks) {
    unsigned index = (unsigned)__builtin_ctz(blocks);
    struct fd_chunk *chunk = fd_chunk_at(&domain->files, index);
    uint32_t bits = action == CLOSE_EXEC_FDS ? chunk->cloexec : chunk->occupied;
    if (index == low) { bits &= UINT32_MAX << (first % FDS_PER_CHUNK); }
    if (index == high) { bits &= UINT32_MAX >> (31 - last % FDS_PER_CHUNK); }
    if (action == MARK_CLOEXEC) { chunk->cloexec |= bits; }
    else {
      while (bits) {
        int fd = (int)(index * FDS_PER_CHUNK + (unsigned)__builtin_ctz(bits));
        cell_assign_fd(domain, fd, NULL, false);
        bits &= bits - 1;
      }
    }
    blocks &= blocks - 1;
  }
}

void cell_close_fd_range(unsigned first, unsigned last, bool cloexec) {
  apply_fd_range(cell_current_domain_internal(), first, last, cloexec ? MARK_CLOEXEC : CLOSE_FDS);
}

void cell_close_all_fds(struct domain *domain) { apply_fd_range(domain, 0, MAX_FDS - 1, CLOSE_FDS); }

void cell_close_exec_fds(struct domain *domain) { apply_fd_range(domain, 0, MAX_FDS - 1, CLOSE_EXEC_FDS); }

bool cell_copy_fd_table(struct domain *dst, const struct domain *src) {
  uint32_t blocks = src->files.nonempty;
  while (blocks) {
    unsigned i = (unsigned)__builtin_ctz(blocks);
    struct fd_chunk *chunk = prepare(&dst->files, i);
    if (!chunk) { cell_close_all_fds(dst); return false; }
    *chunk = i ? *src->files.more[i - 1] : src->files.first;
    chunk->reserved = 0;
    dst->files.nonempty |= UINT32_C(1) << i;
    for (uint32_t bits = chunk->occupied; bits; bits &= bits - 1) {
      cell_retain_open_file(chunk->files[__builtin_ctz(bits)]);
    }
    blocks &= blocks - 1;
  }
  return true;
}

int cell_fd_dup(int oldfd, int minfd) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = cell_file_at(domain, oldfd);
  if (!file) { return -EBADF; }
  if (minfd < 0 || (unsigned)minfd >= domain->nofile_soft) { return -EINVAL; }
  int fd = cell_find_free_fd(domain, minfd);
  if (fd < 0) { return fd; }
  cell_retain_open_file(file);
  cell_assign_fd(domain, fd, file, false);
  return fd;
}

int cell_fd_dup3(int oldfd, int newfd, int flags) {
  if (oldfd == newfd || (flags & ~O_CLOEXEC)) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = cell_file_at(domain, oldfd);
  if (!file || (unsigned)newfd >= domain->nofile_soft) { return -EBADF; }
  struct fd_chunk *chunk = prepare(&domain->files, (unsigned)newfd / FDS_PER_CHUNK);
  if (!chunk) { return -ENOMEM; }
  if (chunk->reserved & (UINT32_C(1) << (newfd % FDS_PER_CHUNK))) { return -16; }
  cell_retain_open_file(file);
  cell_assign_fd(domain, newfd, file, flags & O_CLOEXEC);
  return newfd;
}

int cell_fd_get_fd_flags(int fd) {
  struct domain *domain = cell_current_domain_internal();
  if (!cell_file_at(domain, fd)) { return -EBADF; }
  return (fd_chunk_at(&domain->files, (unsigned)fd / FDS_PER_CHUNK)->cloexec >> (fd % FDS_PER_CHUNK)) & 1;
}

int cell_fd_set_fd_flags(int fd, int flags) {
  struct domain *domain = cell_current_domain_internal();
  if (!cell_file_at(domain, fd)) { return -EBADF; }
  struct fd_chunk *chunk = fd_chunk_at(&domain->files, (unsigned)fd / FDS_PER_CHUNK);
  uint32_t bit = UINT32_C(1) << (fd % FDS_PER_CHUNK);
  if (flags & 1) { chunk->cloexec |= bit; }
  else { chunk->cloexec &= ~bit; }
  return 0;
}
