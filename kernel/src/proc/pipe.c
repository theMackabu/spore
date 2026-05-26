#include "proc/pipe.h"

#include "mm/vmm.h"

enum {
  EAGAIN = 11,
  EFAULT = 14,
  EPIPE = 32,
};

struct pipe_obj {
  bool used;
  bool had_writer;
  uint16_t readers;
  uint16_t writers;
  uint64_t fifo_ino;
  uint8_t data[4096];
  uint64_t head;
  uint64_t len;
};

static struct pipe_obj pipes[16];

static struct pipe_obj *pipe_for_id(uint8_t pipe_id) {
  if (pipe_id >= sizeof(pipes) / sizeof(pipes[0]) || !pipes[pipe_id].used) { return NULL; }
  return &pipes[pipe_id];
}

static struct pipe_obj *pipe_for_file(struct open_file *file) {
  if (file == NULL || file->type != OPEN_PIPE) { return NULL; }
  return pipe_for_id(file->pipe_id);
}

int cell_alloc_pipe_obj(uint64_t fifo_ino) {
  for (size_t i = 0; i < sizeof(pipes) / sizeof(pipes[0]); ++i) {
    if (!pipes[i].used) {
      pipes[i] = (struct pipe_obj){0};
      pipes[i].used = true;
      pipes[i].fifo_ino = fifo_ino;
      pipes[i].had_writer = fifo_ino == 0;
      return (int)i;
    }
  }
  return -1;
}

void cell_pipe_free(uint8_t pipe_id) {
  if (pipe_id < sizeof(pipes) / sizeof(pipes[0])) { pipes[pipe_id].used = false; }
}

void cell_pipe_reset(void) {
  for (size_t i = 0; i < sizeof(pipes) / sizeof(pipes[0]); ++i) {
    pipes[i] = (struct pipe_obj){0};
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
  if (pipe->readers == 0 && pipe->writers == 0) { pipe->used = false; }
}

void cell_pipe_drop_writer(uint8_t pipe_id) {
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  if (pipe == NULL) { return; }
  if (pipe->writers > 0) { --pipe->writers; }
  if (pipe->readers == 0 && pipe->writers == 0) { pipe->used = false; }
}

bool cell_pipe_file_readable(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  return pipe != NULL && !file->pipe_write_end && (pipe->len != 0 || pipe->writers == 0);
}

bool cell_pipe_file_writable(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  return pipe != NULL && file->pipe_write_end && pipe->readers != 0 && pipe->len < sizeof(pipe->data);
}

bool cell_pipe_id_readable(uint8_t pipe_id) {
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  return pipe != NULL && (pipe->len != 0 || pipe->writers == 0);
}

bool cell_pipe_id_writable(uint8_t pipe_id) {
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  return pipe != NULL && pipe->readers != 0 && pipe->len < sizeof(pipe->data);
}

bool cell_pipe_release_file(struct open_file *file) {
  struct pipe_obj *pipe = pipe_for_file(file);
  if (pipe == NULL) { return false; }
  if (file->pipe_write_end) {
    if (pipe->writers > 0) { --pipe->writers; }
  } else if (pipe->readers > 0) {
    --pipe->readers;
  }
  if (pipe->readers == 0 && pipe->writers == 0) { pipe->used = false; }
  return true;
}

int64_t cell_pipe_write_id_from_domain(struct domain *domain, uint8_t pipe_id, uint64_t buf, uint64_t len) {
  if (len == 0) { return 0; }
  struct pipe_obj *pipe = pipe_for_id(pipe_id);
  if (pipe == NULL) { return -9; }
  if (pipe->readers == 0) { return -EPIPE; }
  uint64_t done = 0;
  while (done < len && pipe->len < sizeof(pipe->data)) {
    char c;
    if (!vmm_copy_from_user(&domain->as, &c, buf + done, 1)) { return -EFAULT; }
    uint64_t tail = (pipe->head + pipe->len) % sizeof(pipe->data);
    pipe->data[tail] = (uint8_t)c;
    ++pipe->len;
    ++done;
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
  uint64_t done = 0;
  while (done < len && pipe->len != 0) {
    char c = (char)pipe->data[pipe->head];
    pipe->head = (pipe->head + 1u) % sizeof(pipe->data);
    --pipe->len;
    if (!vmm_copy_to_user(&domain->as, buf + done, &c, 1)) { return -EFAULT; }
    ++done;
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
