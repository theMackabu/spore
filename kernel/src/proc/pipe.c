#include "proc/pipe.h"

#include "mm/pmm.h"
#include "mm/vmm.h"
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

struct pipe_obj {
  bool used;
  bool had_writer;
  uint16_t readers;
  uint16_t writers;
  uint64_t fifo_ino;
  uint64_t pages[PIPE_PAGES];
  uint64_t head;
  uint64_t len;
};

static struct pipe_obj pipes[16];
static bool pipe_waking;

static struct pipe_obj *pipe_for_id(uint8_t pipe_id) {
  if (pipe_id >= sizeof(pipes) / sizeof(pipes[0]) || !pipes[pipe_id].used) { return NULL; }
  return &pipes[pipe_id];
}

static struct pipe_obj *pipe_for_file(struct open_file *file) {
  if (file == NULL || file->type != OPEN_PIPE) { return NULL; }
  return pipe_for_id(file->pipe_id);
}

static void pipe_free_storage(struct pipe_obj *pipe) {
  if (pipe == NULL) { return; }
  for (size_t i = 0; i < PIPE_PAGES; ++i) {
    if (pipe->pages[i] != 0) {
      pmm_free_page(pipe->pages[i]);
      pipe->pages[i] = 0;
    }
  }
}

static void pipe_destroy(struct pipe_obj *pipe) {
  if (pipe == NULL) { return; }
  pipe_free_storage(pipe);
  *pipe = (struct pipe_obj){0};
}

static bool pipe_alloc_storage(struct pipe_obj *pipe) {
  for (size_t i = 0; i < PIPE_PAGES; ++i) {
    pipe->pages[i] = pmm_alloc_zero_page();
    if (pipe->pages[i] == 0) {
      pipe_free_storage(pipe);
      return false;
    }
  }
  return true;
}

static uint8_t *pipe_data_ptr(const struct pipe_obj *pipe, const struct domain *domain, uint64_t pos) {
  if (pipe == NULL || domain == NULL || pos >= PIPE_CAP) { return NULL; }
  uint64_t page = pos / PAGE_SIZE;
  uint64_t off = pos % PAGE_SIZE;
  if (page >= PIPE_PAGES || pipe->pages[page] == 0) { return NULL; }
  const struct user_address_space *as = cell_domain_as_const(domain);
  if (as == NULL) { return NULL; }
  return (uint8_t *)(uintptr_t)(as->hhdm_offset + pipe->pages[page] + off);
}

int cell_alloc_pipe_obj(uint64_t fifo_ino) {
  for (size_t i = 0; i < sizeof(pipes) / sizeof(pipes[0]); ++i) {
    if (!pipes[i].used) {
      pipes[i] = (struct pipe_obj){0};
      if (!pipe_alloc_storage(&pipes[i])) { return -1; }
      pipes[i].used = true;
      pipes[i].fifo_ino = fifo_ino;
      pipes[i].had_writer = fifo_ino == 0;
      return (int)i;
    }
  }
  return -1;
}

void cell_pipe_free(uint8_t pipe_id) {
  if (pipe_id < sizeof(pipes) / sizeof(pipes[0])) { pipe_destroy(&pipes[pipe_id]); }
}

void cell_pipe_reset(void) {
  for (size_t i = 0; i < sizeof(pipes) / sizeof(pipes[0]); ++i) {
    pipe_destroy(&pipes[i]);
  }
}

int cell_fifo_pipe_for_ino(uint64_t ino, bool create) {
  if (ino == 0) { return -1; }
  for (size_t i = 0; i < sizeof(pipes) / sizeof(pipes[0]); ++i) {
    if (pipes[i].used && pipes[i].fifo_ino == ino) { return (int)i; }
  }
  return create ? cell_alloc_pipe_obj(ino) : -1;
}

bool cell_pipe_has_readers(uint8_t pipe_id) {
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  return pipe != NULL && pipe->readers != 0;
}

void cell_pipe_add_reader(uint8_t pipe_id) {
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  if (pipe != NULL) { ++pipe->readers; }
}

void cell_pipe_add_writer(uint8_t pipe_id) {
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  if (pipe != NULL) {
    ++pipe->writers;
    pipe->had_writer = true;
  }
}

void cell_pipe_drop_reader(uint8_t pipe_id) {
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  if (pipe == NULL) { return; }
  if (pipe->readers > 0) { --pipe->readers; }
  if (pipe->readers == 0 && pipe->writers == 0) { pipe_destroy(pipe); }
}

void cell_pipe_drop_writer(uint8_t pipe_id) {
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  if (pipe == NULL) { return; }
  if (pipe->writers > 0) { --pipe->writers; }
  if (pipe->readers == 0 && pipe->writers == 0) { pipe_destroy(pipe); }
}

bool cell_pipe_file_readable(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  return pipe != NULL && !file->pipe_write_end && (pipe->len != 0 || pipe->writers == 0);
}

bool cell_pipe_file_writable(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  return pipe != NULL && file->pipe_write_end && pipe->readers != 0 && pipe->len < PIPE_CAP;
}

bool cell_pipe_file_hup(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  return pipe != NULL && !file->pipe_write_end && pipe->writers == 0;
}

bool cell_pipe_id_readable(uint8_t pipe_id) {
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  return pipe != NULL && (pipe->len != 0 || pipe->writers == 0);
}

bool cell_pipe_id_writable(uint8_t pipe_id) {
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  return pipe != NULL && pipe->readers != 0 && pipe->len < PIPE_CAP;
}

bool cell_pipe_id_hup(uint8_t pipe_id) {
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  return pipe != NULL && pipe->writers == 0;
}

bool cell_pipe_release_file(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  if (pipe == NULL) { return false; }
  if (file->pipe_write_end) {
    if (pipe->writers > 0) { --pipe->writers; }
  } else if (pipe->readers > 0) {
    --pipe->readers;
  }
  if (pipe->readers == 0 && pipe->writers == 0) { pipe_destroy(pipe); }
  return true;
}

static void clear_pipe_wait(struct thread *thread) {
  thread->wait_target = -1;
  thread->pipe_buf = 0;
  thread->pipe_len = 0;
  thread->socket_addr = 0;
  thread->socket_addrlen = 0;
  thread->pipe_write = false;
}

static void wake_pipe_waiters(void) {
  if (pipe_waking) { return; }
  pipe_waking = true;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_PIPE ||
        thread->domain == NULL) {
      continue;
    }
    int fd = thread->wait_target;
    if (fd < 0 || fd >= MAX_FDS || thread->domain->fds[fd] == NULL) {
      thread->tf.x[0] = (uint64_t)-9;
    } else {
      struct open_file *file = thread->domain->fds[fd];
      int64_t rc;
      if (file->type == OPEN_UNIX_STREAM) {
        rc = thread->pipe_write
               ? cell_pipe_write_id_from_domain(thread->domain, file->unix_tx_pipe, thread->pipe_buf, thread->pipe_len)
               : cell_pipe_read_id_to_domain(thread->domain, file->unix_rx_pipe, thread->pipe_buf, thread->pipe_len);
      } else {
        rc = thread->pipe_write ? cell_pipe_write_from_domain(thread->domain, file, thread->pipe_buf, thread->pipe_len)
                                : cell_pipe_read_to_domain(thread->domain, file, thread->pipe_buf, thread->pipe_len);
      }
      if (rc == -EAGAIN) { continue; }
      thread->tf.x[0] = (uint64_t)rc;
    }
    thread->state = THREAD_RUNNABLE;
    thread->wait_reason = WAIT_NONE;
    clear_pipe_wait(thread);
  }
  pipe_waking = false;
}

void cell_pipe_notify(void) {
  wake_pipe_waiters();
  cell_wake_poll_waiters_internal();
}

int64_t cell_pipe_write_id_from_domain(struct domain *domain, uint8_t pipe_id, uint64_t buf, uint64_t len) {
  if (len == 0) { return 0; }
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  if (pipe == NULL) { return -9; }
  if (pipe->readers == 0) { return -EPIPE; }

  uint64_t room = PIPE_CAP - pipe->len;
  if (room == 0 || (len <= PIPE_BUF && room < len)) { return -EAGAIN; }
  uint64_t target = len < room ? len : room;
  if (!cell_domain_ensure_user_range(domain, buf, (size_t)target, VMM_ACCESS_READ)) { return -EFAULT; }
  uint64_t done = 0;
  while (done < target) {
    uint64_t tail = (pipe->head + pipe->len) % PIPE_CAP;
    uint64_t chunk = PIPE_CAP - tail;
    uint64_t page_room = PAGE_SIZE - (tail % PAGE_SIZE);
    if (chunk > page_room) { chunk = page_room; }
    if (chunk > target - done) { chunk = target - done; }
    uint8_t *dst = pipe_data_ptr(pipe, domain, tail);
    if (dst == NULL || !vmm_copy_from_user(cell_domain_as(domain), dst, buf + done, (size_t)chunk)) { return -EFAULT; }
    pipe->len += chunk;
    done += chunk;
  }
  if (done != 0) {
    cell_pipe_notify();
    return (int64_t)done;
  }
  return -EAGAIN;
}

int64_t cell_pipe_read_id_to_domain(struct domain *domain, uint8_t pipe_id, uint64_t buf, uint64_t len) {
  if (len == 0) { return 0; }
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  if (pipe == NULL) { return -9; }
  uint64_t target = len < pipe->len ? len : pipe->len;
  if (target != 0 && !cell_domain_ensure_user_range(domain, buf, (size_t)target, VMM_ACCESS_WRITE)) { return -EFAULT; }
  uint64_t done = 0;
  while (done < target) {
    uint64_t chunk = PIPE_CAP - pipe->head;
    uint64_t page_room = PAGE_SIZE - (pipe->head % PAGE_SIZE);
    if (chunk > page_room) { chunk = page_room; }
    if (chunk > target - done) { chunk = target - done; }
    uint8_t *src = pipe_data_ptr(pipe, domain, pipe->head);
    if (src == NULL || !vmm_copy_to_user(cell_domain_as(domain), buf + done, src, (size_t)chunk)) { return -EFAULT; }
    pipe->head = (pipe->head + chunk) % PIPE_CAP;
    pipe->len -= chunk;
    done += chunk;
  }
  if (done != 0) {
    cell_pipe_notify();
    return (int64_t)done;
  }
  return pipe->writers == 0 && (pipe->fifo_ino == 0 || pipe->had_writer) ? 0 : -EAGAIN;
}

int64_t cell_pipe_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (file == NULL || file->type != OPEN_PIPE || !file->pipe_write_end) { return -9; }
  return cell_pipe_write_id_from_domain(domain, file->pipe_id, buf, len);
}

int64_t cell_pipe_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (file == NULL || file->type != OPEN_PIPE || file->pipe_write_end) { return -9; }
  return cell_pipe_read_id_to_domain(domain, file->pipe_id, buf, len);
}
