#include "proc/pipe.h"

#include "mm/pmm.h"
#include "mm/pool.h"
#include "mm/page_ring.h"
#include "mm/vmm.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/poll.h"
#include "net/socket.h"
#include "proc/thread.h"

enum {
  EAGAIN = 11,
  EFAULT = 14,
  EPIPE = 32,
  PIPE_BUF = 4096,
  PIPE_CAP = 65536,
  PIPE_PAGES = PIPE_CAP / PAGE_SIZE,
};

static_assert(PIPE_CAP % PAGE_SIZE == 0 && PIPE_PAGES <= 64);

struct pipe_obj {
  struct pipe_obj *fifo_next;
  bool had_writer;
  uint16_t readers;
  uint16_t writers;
  uint64_t ino, fifo_dev;
  uint64_t pages[PIPE_PAGES];
  struct page_ring ring;
  uint64_t read_offset;
  uint64_t write_offset;
};

static struct object_pool pipes;
static struct pipe_obj *fifos;
static uint64_t next_pipe_ino = 20;
static bool pipe_waking;

static struct pipe_obj *pipe_for_file(struct open_file *file) {
  return file && file->type == OPEN_PIPE ? file->pipe : NULL;
}

void cell_pipe_discard(struct pipe_obj *pipe) {
  if (!pipe) { return; }
  if (pipe->fifo_dev) {
    struct pipe_obj **link = &fifos;
    while (*link != pipe) { link = &(*link)->fifo_next; }
    *link = pipe->fifo_next;
  }
  cell_unix_release_rights_for_pipe(pipe);
  page_ring_clear(&pipe->ring);
  pool_free(&pipes, pipe);
}

struct pipe_obj *cell_pipe_create(void) {
  if (!pipes.limit && !pool_init(&pipes, sizeof(struct pipe_obj), SIZE_MAX)) { return NULL; }
  struct pipe_obj *pipe = pool_alloc(&pipes);
  if (pipe) {
    pipe->ring = (struct page_ring){.pages = pipe->pages, .capacity = PIPE_CAP};
    pipe->ino = next_pipe_ino++;
    pipe->had_writer = true;
  }
  return pipe;
}

struct pipe_obj *cell_fifo_open(const struct vfs_node *node, bool create) {
  for (struct pipe_obj *pipe = fifos; pipe; pipe = pipe->fifo_next) {
    if (pipe->fifo_dev == node->dev_id && pipe->ino == node->ino) { return pipe; }
  }
  struct pipe_obj *pipe = create ? cell_pipe_create() : NULL;
  if (pipe) {
    pipe->fifo_next = fifos;
    fifos = pipe;
    pipe->fifo_dev = node->dev_id;
    pipe->ino = node->ino;
    pipe->had_writer = false;
  }
  return pipe;
}

uint64_t cell_pipe_ino(const struct pipe_obj *pipe) { return pipe->ino; }

bool cell_pipe_has_readers(struct pipe_obj *pipe) {
  return pipe != NULL && pipe->readers != 0;
}

void cell_pipe_add_reader(struct pipe_obj *pipe) {
  if (pipe != NULL) { ++pipe->readers; }
}

void cell_pipe_add_writer(struct pipe_obj *pipe) {
  if (pipe != NULL) {
    ++pipe->writers;
    pipe->had_writer = true;
  }
}

void cell_pipe_drop_reader(struct pipe_obj *pipe) {
  if (pipe == NULL) { return; }
  if (pipe->readers > 0) { --pipe->readers; }
  if (pipe->readers == 0 && pipe->writers == 0) { cell_pipe_discard(pipe); }
}

void cell_pipe_drop_writer(struct pipe_obj *pipe) {
  if (pipe == NULL) { return; }
  if (pipe->writers > 0) { --pipe->writers; }
  if (pipe->readers == 0 && pipe->writers == 0) { cell_pipe_discard(pipe); }
}

bool cell_pipe_file_readable(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  return pipe != NULL && !file->pipe_write_end && (pipe->ring.used != 0 || pipe->writers == 0);
}

bool cell_pipe_file_writable(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  return pipe != NULL && file->pipe_write_end && pipe->readers != 0 && pipe->ring.used < PIPE_CAP;
}

bool cell_pipe_file_hup(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  return pipe != NULL && !file->pipe_write_end && pipe->writers == 0;
}

bool cell_pipe_readable(struct pipe_obj *pipe) {
  return pipe != NULL && (pipe->ring.used != 0 || pipe->writers == 0);
}

bool cell_pipe_writable(struct pipe_obj *pipe) {
  return pipe != NULL && pipe->readers != 0 && pipe->ring.used < PIPE_CAP;
}

bool cell_pipe_hup(struct pipe_obj *pipe) {
  return pipe != NULL && pipe->writers == 0;
}

bool cell_pipe_read_offset(struct pipe_obj *pipe, uint64_t *out) {
  if (pipe == NULL || out == NULL) { return false; }
  *out = pipe->read_offset;
  return true;
}

bool cell_pipe_write_offset(struct pipe_obj *pipe, uint64_t *out) {
  if (pipe == NULL || out == NULL) { return false; }
  *out = pipe->write_offset;
  return true;
}

bool cell_pipe_release_file(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  if (pipe == NULL) { return false; }
  if (file->pipe_write_end) {
    if (pipe->writers > 0) { --pipe->writers; }
  } else if (pipe->readers > 0) {
    --pipe->readers;
  }
  if (pipe->readers == 0 && pipe->writers == 0) { cell_pipe_discard(pipe); }
  return true;
}

static void wake_pipe_waiters(void) {
  if (pipe_waking) { return; }
  pipe_waking = true;
  size_t cursor = 0;
  struct thread *thread;
  while ((thread = cell_next_waiter(WAIT_PIPE, &cursor))) {
    int fd = thread->wait.target;
    if (fd < 0 || fd >= MAX_FDS || thread->domain->fds[fd] == NULL) {
      cell_finish_wait(thread, -9);
    } else {
      struct open_file *file = thread->domain->fds[fd];
      int64_t rc;
      if (file->type == OPEN_UNIX_STREAM) {
        rc = thread->wait.write
               ? cell_pipe_write_user(thread->domain, file->unix_tx_pipe, thread->wait.buf, thread->wait.len)
               : cell_pipe_read_user(thread->domain, file->unix_rx_pipe, thread->wait.buf, thread->wait.len);
      } else {
        rc = thread->wait.write ? cell_pipe_write_from_domain(thread->domain, file, thread->wait.buf, thread->wait.len)
                                : cell_pipe_read_to_domain(thread->domain, file, thread->wait.buf, thread->wait.len);
      }
      if (rc == -EAGAIN) { continue; }
      cell_finish_wait(thread, rc);
    }
  }
  pipe_waking = false;
}

void cell_pipe_notify(void) {
  wake_pipe_waiters();
  cell_wake_poll_waiters_internal();
}

struct pipe_user_buffer {
  struct user_address_space *as;
  uint64_t addr;
};

static int read_user(void *source, uint8_t *dst, size_t len) {
  struct pipe_user_buffer *buf = source;
  if (!vmm_copy_from_user(buf->as, dst, buf->addr, len)) { return -EFAULT; }
  buf->addr += len;
  return 0;
}

static int write_user(void *destination, const uint8_t *src, size_t len) {
  struct pipe_user_buffer *buf = destination;
  if (!vmm_copy_to_user(buf->as, buf->addr, src, len)) { return -EFAULT; }
  buf->addr += len;
  return 0;
}

int64_t cell_pipe_write_user(struct domain *domain, struct pipe_obj *pipe, uint64_t buf, uint64_t len) {
  if (len == 0) { return 0; }
  if (pipe == NULL) { return -9; }
  if (pipe->readers == 0) { return -EPIPE; }

  uint64_t room = PIPE_CAP - pipe->ring.used;
  if (room == 0 || (len <= PIPE_BUF && room < len)) { return -EAGAIN; }
  uint64_t target = len < room ? len : room;
  if (!cell_domain_ensure_user_range(domain, buf, (size_t)target, VMM_ACCESS_READ)) { return -EFAULT; }
  struct pipe_user_buffer source = {cell_domain_as(domain), buf};
  int n = page_ring_write(&pipe->ring, (uint32_t)target, read_user, &source);
  if (n > 0) {
    pipe->write_offset += (uint32_t)n;
    cell_pipe_notify();
  }
  return n;
}

int64_t cell_pipe_read_user(struct domain *domain, struct pipe_obj *pipe, uint64_t buf, uint64_t len) {
  if (len == 0) { return 0; }
  if (pipe == NULL) { return -9; }
  uint64_t target = len < pipe->ring.used ? len : pipe->ring.used;
  if (!target) { return pipe->writers == 0 && (!pipe->fifo_dev || pipe->had_writer) ? 0 : -EAGAIN; }
  if (!cell_domain_ensure_user_range(domain, buf, (size_t)target, VMM_ACCESS_WRITE)) { return -EFAULT; }
  struct pipe_user_buffer dst = {cell_domain_as(domain), buf};
  int n = page_ring_read(&pipe->ring, (uint32_t)target, false, write_user, &dst);
  if (n > 0) {
    pipe->read_offset += (uint32_t)n;
    cell_pipe_notify();
  }
  return n;
}

int64_t cell_pipe_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (file == NULL || file->type != OPEN_PIPE || !file->pipe_write_end) { return -9; }
  return cell_pipe_write_user(domain, file->pipe, buf, len);
}

int64_t cell_pipe_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (file == NULL || file->type != OPEN_PIPE || file->pipe_write_end) { return -9; }
  return cell_pipe_read_user(domain, file->pipe, buf, len);
}
