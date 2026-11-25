#include "proc/pipe.h"
#include "abi/linux/shared/open.h"

#include "abi/linux/shared/unix.h"
#include "mm/page_ring.h"
#include "mm/pmm.h"
#include "mm/pool.h"
#include "mm/vmm.h"
#include "net/unix.h"
#include "proc/domain.h"
#include "proc/epoll.h"
#include "proc/fd.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/poll.h"
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

struct packet_record {
  struct packet_record *next;
  uint32_t size;
};

struct pipe_obj {
  bool packet;
  struct packet_record *first, *last;
  struct pipe_obj *fifo_next;
  bool had_writer;
  uint16_t readers;
  uint16_t writers;
  uint64_t ino, fifo_dev;
  uint64_t pages[PIPE_PAGES];
  struct page_ring ring;
  struct unix_rights_queue rights;
  struct epoll_source poll_source;
  uint64_t read_offset;
  uint64_t write_offset;
};

static struct object_pool pipes, packets;
static struct pipe_obj *fifos;
static uint64_t next_pipe_ino = 20;
static bool pipe_waking, pipe_rescan;

static struct pipe_obj *pipe_for_file(struct open_file *file) {
  return file && file->type == OPEN_PIPE ? file->pipe : NULL;
}

void cell_pipe_discard(struct pipe_obj *pipe) {
  if (!pipe) { return; }
  if (pipe->fifo_dev) {
    struct pipe_obj **link = &fifos;
    while (*link != pipe) {
      link = &(*link)->fifo_next;
    }
    *link = pipe->fifo_next;
  }
  unix_clear_rights(&pipe->rights);
  while (pipe->first) {
    struct packet_record *record = pipe->first;
    pipe->first = record->next;
    pool_free(&packets, record);
  }
  page_ring_clear(&pipe->ring);
  pool_free(&pipes, pipe);
}

bool cell_pipe_is_packet(const struct pipe_obj *pipe) {
  return pipe && pipe->packet;
}

bool cell_pipe_packet_size(const struct pipe_obj *pipe, uint32_t *size) {
  if (!pipe || !pipe->first) { return false; }
  *size = pipe->first->size;
  return true;
}

struct pipe_obj *cell_pipe_create_mode(bool packet) {
  if (!pipes.limit && !pool_init(&pipes, sizeof(struct pipe_obj), SIZE_MAX)) { return NULL; }
  struct pipe_obj *pipe = pool_alloc(&pipes);
  if (pipe) {
    pipe->packet = packet;
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

uint64_t cell_pipe_ino(const struct pipe_obj *pipe) {
  return pipe->ino;
}

bool cell_fifo_has_peer(const struct open_file *file) {
  return linux_open_readable(file->flags) ? file->pipe->writers != 0 : file->pipe->readers != 0;
}

bool cell_pipe_has_readers(struct pipe_obj *pipe) {
  return pipe != NULL && pipe->readers != 0;
}

void cell_pipe_add_reader(struct pipe_obj *pipe) {
  if (pipe) {
    ++pipe->readers;
    cell_epoll_source_changed(&pipe->poll_source);
  }
}

void cell_pipe_add_writer(struct pipe_obj *pipe) {
  if (pipe != NULL) {
    ++pipe->writers;
    pipe->had_writer = true;
    cell_epoll_source_changed(&pipe->poll_source);
  }
}

void cell_pipe_drop_reader(struct pipe_obj *pipe) {
  if (!pipe || !pipe->readers || --pipe->readers) { return; }
  cell_epoll_source_changed(&pipe->poll_source);
  struct unix_rights_queue abandoned = pipe->rights;
  pipe->rights = (struct unix_rights_queue){0};
  if (!pipe->writers) { cell_pipe_discard(pipe); }
  unix_clear_rights(&abandoned);
}

void cell_pipe_drop_writer(struct pipe_obj *pipe) {
  if (pipe == NULL) { return; }
  if (pipe->writers > 0) { --pipe->writers; }
  cell_epoll_source_changed(&pipe->poll_source);
  if (pipe->readers == 0 && pipe->writers == 0) { cell_pipe_discard(pipe); }
}

bool cell_pipe_file_readable(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  return pipe != NULL && linux_open_readable(file->flags) && pipe->ring.used != 0;
}

bool cell_pipe_file_writable(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  return pipe != NULL && linux_open_writable(file->flags) && pipe->ring.used < PIPE_CAP;
}

bool cell_pipe_file_hup(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  return pipe != NULL && linux_open_readable(file->flags) && pipe->had_writer && pipe->writers == 0;
}

bool cell_pipe_file_error(struct open_file *file) {
  return file->type == OPEN_PIPE && linux_open_writable(file->flags) && !file->pipe->readers;
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

struct epoll_source *cell_pipe_poll_source(struct pipe_obj *pipe) {
  return &pipe->poll_source;
}
uint64_t cell_pipe_read_offset(const struct pipe_obj *pipe) {
  return pipe->read_offset;
}
struct unix_rights_queue *cell_pipe_rights(struct pipe_obj *pipe) {
  return &pipe->rights;
}

bool cell_pipe_release_file(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  if (pipe == NULL) { return false; }
  if (linux_open_readable(file->flags)) { cell_pipe_drop_reader(pipe); }
  if (linux_open_writable(file->flags)) { cell_pipe_drop_writer(pipe); }
  return true;
}

static void wake_pipe_waiters(void) {
  if (pipe_waking) {
    pipe_rescan = true;
    return;
  }
  pipe_waking = true;
  do {
    pipe_rescan = false;
    size_t cursor = 0;
    struct thread *thread;
    while ((thread = cell_next_waiter(WAIT_FIFO_OPEN, &cursor))) {
      struct open_file *file = thread->wait.held_file;
      if (thread->domain->zombie || !cell_fifo_has_peer(file)) { continue; }
      cell_retain_open_file(file);
      int fd = thread->wait.target;
      cell_assign_fd(thread->domain, fd, file, thread->wait.fifo_cloexec);
      fsnotify_opened(file);
      cell_finish_wait(thread, fd);
    }
    cursor = 0;
    while ((thread = cell_next_waiter(WAIT_PIPE, &cursor))) {
      struct open_file *file = thread->wait.held_file;
      int64_t rc;
      if (thread->wait.socket_vector) {
        rc = linux_unix_vector(thread->domain, file, thread->wait.socket_iov, thread->wait.socket_iovlen,
                               thread->wait.write, NULL);
      } else if (thread->wait.socket_msg) {
        rc = linux_unix_transfer(thread->domain, file, thread->wait.socket_msg_addr, thread->wait.socket_flags,
                                 thread->wait.write, &thread->wait.unix_rights);
      } else if (file->type == OPEN_UNIX_STREAM) {
        rc = thread->wait.write
               ? cell_pipe_write_user(thread->domain, file->unix_tx_pipe, thread->wait.buf, thread->wait.len)
               : cell_pipe_recv_user(thread->domain, file->unix_rx_pipe, thread->wait.buf, thread->wait.len,
                                     thread->wait.socket_flags);
      } else {
        rc = thread->wait.write ? cell_pipe_write_from_domain(thread->domain, file, thread->wait.buf, thread->wait.len)
                                : cell_pipe_read_to_domain(thread->domain, file, thread->wait.buf, thread->wait.len);
      }
      if (rc == -EAGAIN) { continue; }
      cell_finish_wait(thread, rc);
    }
  } while (pipe_rescan);
  pipe_waking = false;
}

void cell_pipe_notify(void) {
  wake_pipe_waiters();
  cell_wake_poll_waiters_internal();
}

struct pipe_user_buffer {
  struct domain *domain;
  uint64_t addr;
};

static int read_user(void *source, uint8_t *dst, size_t len) {
  struct pipe_user_buffer *buf = source;
  if (!vmm_copy_from_user(cell_domain_as(buf->domain), dst, buf->addr, len) &&
      !(cell_domain_ensure_user_range(buf->domain, buf->addr, len, VMM_ACCESS_READ) &&
        vmm_copy_from_user(cell_domain_as(buf->domain), dst, buf->addr, len))) {
    return -EFAULT;
  }
  buf->addr += len;
  return 0;
}

static int write_user(void *destination, const uint8_t *src, size_t len) {
  struct pipe_user_buffer *buf = destination;
  if (!vmm_copy_to_user(cell_domain_as(buf->domain), buf->addr, src, len) &&
      !(cell_domain_ensure_user_range(buf->domain, buf->addr, len, VMM_ACCESS_WRITE) &&
        vmm_copy_to_user(cell_domain_as(buf->domain), buf->addr, src, len))) {
    return -EFAULT;
  }
  buf->addr += len;
  return 0;
}

static int empty_record_byte(void *ctx, uint8_t *dst, size_t len) {
  (void)ctx;
  (void)len;
  *dst = 0;
  return 0;
}

int64_t cell_pipe_write_data(struct pipe_obj *pipe, uint64_t len, ring_copy_in copy, void *source,
                             struct unix_rights **rights) {
  if (!pipe) { return -9; }
  if (!len && !pipe->packet) { return 0; }
  if (!pipe->readers) { return -EPIPE; }
  if (pipe->packet && len > PIPE_CAP) { return -90; }
  uint64_t stored = len ? len : 1;
  uint32_t room = PIPE_CAP - pipe->ring.used;
  if (!room || ((pipe->packet || len <= PIPE_BUF) && room < stored)) { return -EAGAIN; }
  struct packet_record *record = NULL;
  if (pipe->packet) {
    if (!packets.limit) { pool_init(&packets, sizeof(*record), SIZE_MAX); }
    record = pool_alloc(&packets);
    if (!record) { return -12; }
    record->size = len;
  }
  int n = page_ring_write(&pipe->ring, stored < room ? (uint32_t)stored : room, len ? copy : empty_record_byte, source);
  if (n <= 0 && record) { pool_free(&packets, record); }
  if (n > 0) {
    if (record) {
      if (pipe->last) {
        pipe->last->next = record;
      } else {
        pipe->first = record;
      }
      pipe->last = record;
    }
    uint64_t start = pipe->write_offset;
    pipe->write_offset += (uint32_t)n;
    if (rights && *rights) {
      unix_queue_rights(&pipe->rights, *rights, start, pipe->write_offset);
      *rights = NULL;
    }
    cell_epoll_source_activity(&pipe->poll_source, CELL_POLLIN);
    cell_pipe_notify();
  }
  return !len && n > 0 ? 0 : n;
}

int64_t cell_pipe_peek_data(struct pipe_obj *pipe, uint64_t len, ring_copy_out copy, void *dst) {
  if (!pipe) { return -9; }
  if (!len && !pipe->packet) { return 0; }
  if (!pipe->ring.used) { return !pipe->writers && (!pipe->fifo_dev || pipe->had_writer) ? 0 : -EAGAIN; }
  if (pipe->first && len > pipe->first->size) { len = pipe->first->size; }
  return page_ring_read(&pipe->ring, len < pipe->ring.used ? (uint32_t)len : pipe->ring.used, true, copy, dst);
}

void cell_pipe_consume(struct pipe_obj *pipe, uint32_t bytes) {
  if (pipe->first) {
    struct packet_record *record = pipe->first;
    bytes = record->size ? record->size : 1;
    pipe->first = record->next;
    if (!pipe->first) { pipe->last = NULL; }
    pool_free(&packets, record);
  }
  page_ring_consume(&pipe->ring, bytes);
  pipe->read_offset += bytes;
  unix_discard_rights(&pipe->rights, pipe->read_offset);
  cell_epoll_source_activity(&pipe->poll_source, CELL_POLLOUT);
  cell_pipe_notify();
}

int64_t cell_pipe_write_user(struct domain *domain, struct pipe_obj *pipe, uint64_t buf, uint64_t len) {
  struct pipe_user_buffer src = {domain, buf};
  return cell_pipe_write_data(pipe, len, read_user, &src, NULL);
}

int64_t cell_pipe_recv_user(struct domain *domain, struct pipe_obj *pipe, uint64_t buf, uint64_t len, unsigned flags) {
  struct pipe_user_buffer dst = {domain, buf};
  int64_t n = cell_pipe_peek_data(pipe, len, write_user, &dst);
  uint32_t size = 0;
  bool packet = cell_pipe_packet_size(pipe, &size);
  if (n >= 0 && (n || packet) && !(flags & 2)) { cell_pipe_consume(pipe, (uint32_t)n); }
  return n >= 0 && packet && (flags & 32) ? (int64_t)size : n;
}

int64_t cell_pipe_read_user(struct domain *domain, struct pipe_obj *pipe, uint64_t buf, uint64_t len) {
  return cell_pipe_recv_user(domain, pipe, buf, len, false);
}

int64_t cell_pipe_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (file == NULL || file->type != OPEN_PIPE || !linux_open_writable(file->flags)) { return -9; }
  return cell_pipe_write_user(domain, file->pipe, buf, len);
}

int64_t cell_pipe_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (file == NULL || file->type != OPEN_PIPE || !linux_open_readable(file->flags)) { return -9; }
  if (!file->pipe->writers && !file->pipe->ring.used && (file->flags & 04000)) { return 0; }
  return cell_pipe_read_user(domain, file->pipe, buf, len);
}
