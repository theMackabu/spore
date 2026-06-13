#include "proc/pty.h"

#include "kstr.h"
#include "mem.h"
#include "proc/fd.h"
#include "proc/thread.h"

enum {
  EAGAIN = 11,
  EBUSY = 16,
  EFAULT = 14,
  EINVAL = 22,
  EIO = 5,
  ENOENT = 2,
  ENOTTY = 25,
  PTY_BUF_CAP = 8192,
  TTY_OPOST = 0000001,
  TTY_ONLCR = 0000004,
  TTY_ISIG = 0000001,
  TTY_ICANON = 0000002,
  TTY_ECHO = 0000010,
  CELL_O_CLOEXEC = 02000000,
};

struct pty_ring {
  char data[PTY_BUF_CAP];
  size_t head;
  size_t len;
};

struct pty {
  bool used;
  bool unlocked;
  uint16_t master_refs;
  uint16_t slave_refs;
  uint16_t rows;
  uint16_t cols;
  int foreground_pgrp;
  uint32_t oflag;
  uint32_t lflag;
  uint8_t erase;
  struct pty_ring to_master;
  struct pty_ring to_slave;
};

static struct pty ptys[CELL_PTY_CAP];
static bool pty_waking;

static void pty_notify(void);

static struct pty *pty_for_file(const struct open_file *file) {
  if (file == NULL || file->type != OPEN_PTY || file->pty_id >= CELL_PTY_CAP || !ptys[file->pty_id].used) {
    return NULL;
  }
  return &ptys[file->pty_id];
}

static size_t ring_room(const struct pty_ring *ring) {
  return PTY_BUF_CAP - ring->len;
}

static bool ring_push(struct pty_ring *ring, char c) {
  if (ring->len >= PTY_BUF_CAP) { return false; }
  size_t index = (ring->head + ring->len) % PTY_BUF_CAP;
  ring->data[index] = c;
  ++ring->len;
  return true;
}

static bool ring_pop(struct pty_ring *ring, char *out) {
  if (ring->len == 0) { return false; }
  *out = ring->data[ring->head];
  ring->head = (ring->head + 1u) % PTY_BUF_CAP;
  --ring->len;
  return true;
}

static void append_dec(char *dst, size_t cap, size_t *len, unsigned value) {
  char tmp[10];
  size_t n = 0;
  do {
    tmp[n++] = (char)('0' + (value % 10u));
    value /= 10u;
  } while (value != 0 && n < sizeof(tmp));
  while (n > 0 && *len + 1 < cap) {
    dst[(*len)++] = tmp[--n];
  }
  if (cap != 0) { dst[*len] = '\0'; }
}

static void pty_path(int id, char *out, size_t cap) {
  const char *prefix = "/dev/pts/";
  size_t len = 0;
  while (prefix[len] != '\0' && len + 1 < cap) {
    out[len] = prefix[len];
    ++len;
  }
  if (cap != 0) { out[len] = '\0'; }
  append_dec(out, cap, &len, (unsigned)id);
}

int cell_pty_id_from_path(const char *path) {
  const char *prefix = "/dev/pts/";
  if (path == NULL || !starts_with(path, prefix)) { return -ENOENT; }
  const char *p = path + kstrlen(prefix);
  if (*p == '\0') { return -ENOENT; }
  int value = 0;
  while (*p != '\0') {
    if (*p < '0' || *p > '9') { return -ENOENT; }
    value = value * 10 + (*p - '0');
    if (value >= CELL_PTY_CAP) { return -ENOENT; }
    ++p;
  }
  return value;
}

static int install_pty_fd(struct domain *domain, int id, bool master, uint32_t flags, const char *path) {
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -24; }
  struct open_file *file = cell_alloc_open_file();
  if (file == NULL) { return -12; }
  file->type = OPEN_PTY;
  file->flags = flags & ~(uint32_t)CELL_O_CLOEXEC;
  file->pty_id = (uint8_t)id;
  file->pty_master = master;
  cell_copy_open_path(file, path);
  domain->fds[fd] = file;
  domain->fd_flags[fd] = (flags & CELL_O_CLOEXEC) != 0 ? 1 : 0;
  return fd;
}

int cell_pty_open_master(uint32_t flags, const char *path) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  int id = -1;
  for (int i = 0; i < CELL_PTY_CAP; ++i) {
    if (!ptys[i].used) {
      id = i;
      break;
    }
  }
  if (id < 0) { return -EBUSY; }
  kmemset(&ptys[id], 0, sizeof(ptys[id]));
  ptys[id].used = true;
  ptys[id].master_refs = 1;
  ptys[id].rows = 38;
  ptys[id].cols = 96;
  ptys[id].foreground_pgrp = domain->pgrp_id;
  ptys[id].oflag = TTY_OPOST | TTY_ONLCR;
  ptys[id].lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO;
  ptys[id].erase = 0x7f;
  int fd = install_pty_fd(domain, id, true, flags, path);
  if (fd < 0) { kmemset(&ptys[id], 0, sizeof(ptys[id])); }
  return fd;
}

int cell_pty_open_slave(int id, uint32_t flags, const char *path) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  if (id < 0 || id >= CELL_PTY_CAP || !ptys[id].used) { return -ENOENT; }
  if (!ptys[id].unlocked) { return -EIO; }
  ++ptys[id].slave_refs;
  int fd = install_pty_fd(domain, id, false, flags, path);
  if (fd < 0) { --ptys[id].slave_refs; }
  return fd;
}

int cell_pty_open_peer(struct open_file *master, uint32_t flags) {
  struct pty *pty = pty_for_file(master);
  if (pty == NULL || !master->pty_master) { return -ENOTTY; }
  char path[32];
  pty_path(master->pty_id, path, sizeof(path));
  pty->unlocked = true;
  return cell_pty_open_slave(master->pty_id, flags, path);
}

void cell_pty_release_file(struct open_file *file) {
  struct pty *pty = pty_for_file(file);
  if (pty == NULL) { return; }
  if (file->pty_master) {
    if (pty->master_refs > 0) { --pty->master_refs; }
  } else if (pty->slave_refs > 0) {
    --pty->slave_refs;
  }
  if (pty->master_refs == 0 && pty->slave_refs == 0) { kmemset(pty, 0, sizeof(*pty)); }
  pty_notify();
}

bool cell_pty_file_readable(const struct open_file *file) {
  struct pty *pty = pty_for_file(file);
  if (pty == NULL) { return false; }
  const struct pty_ring *ring = file->pty_master ? &pty->to_master : &pty->to_slave;
  uint16_t peer_refs = file->pty_master ? pty->slave_refs : pty->master_refs;
  return ring->len != 0 || peer_refs == 0;
}

bool cell_pty_file_writable(const struct open_file *file) {
  struct pty *pty = pty_for_file(file);
  if (pty == NULL) { return false; }
  const struct pty_ring *ring = file->pty_master ? &pty->to_slave : &pty->to_master;
  uint16_t peer_refs = file->pty_master ? pty->slave_refs : pty->master_refs;
  return peer_refs != 0 && ring_room(ring) != 0;
}

bool cell_pty_file_hup(const struct open_file *file) {
  struct pty *pty = pty_for_file(file);
  if (pty == NULL) { return true; }
  uint16_t peer_refs = file->pty_master ? pty->slave_refs : pty->master_refs;
  return peer_refs == 0;
}

uint64_t cell_pty_read_available(const struct open_file *file) {
  struct pty *pty = pty_for_file(file);
  if (pty == NULL) { return 0; }
  const struct pty_ring *ring = file->pty_master ? &pty->to_master : &pty->to_slave;
  return ring->len;
}

uint64_t cell_pty_write_pending(const struct open_file *file) {
  struct pty *pty = pty_for_file(file);
  if (pty == NULL) { return 0; }
  const struct pty_ring *ring = file->pty_master ? &pty->to_slave : &pty->to_master;
  return ring->len;
}

int cell_pty_id(const struct open_file *file) {
  return pty_for_file(file) == NULL ? -ENOTTY : file->pty_id;
}

bool cell_pty_is_master(const struct open_file *file) {
  return pty_for_file(file) != NULL && file->pty_master;
}

bool cell_pty_unlocked(const struct open_file *file) {
  struct pty *pty = pty_for_file(file);
  return pty != NULL && pty->unlocked;
}

int cell_pty_set_locked(struct open_file *file, int locked) {
  struct pty *pty = pty_for_file(file);
  if (pty == NULL || !file->pty_master) { return -ENOTTY; }
  pty->unlocked = locked == 0;
  return 0;
}

uint32_t cell_pty_oflag(const struct open_file *file) {
  struct pty *pty = pty_for_file(file);
  return pty == NULL ? 0 : pty->oflag;
}

void cell_pty_set_oflag(struct open_file *file, uint32_t oflag) {
  struct pty *pty = pty_for_file(file);
  if (pty != NULL) { pty->oflag = oflag; }
}

uint32_t cell_pty_lflag(const struct open_file *file) {
  struct pty *pty = pty_for_file(file);
  return pty == NULL ? 0 : pty->lflag;
}

void cell_pty_set_lflag(struct open_file *file, uint32_t lflag) {
  struct pty *pty = pty_for_file(file);
  if (pty != NULL) { pty->lflag = lflag; }
}

uint8_t cell_pty_erase_char(const struct open_file *file) {
  struct pty *pty = pty_for_file(file);
  return pty == NULL ? 0x7f : pty->erase;
}

void cell_pty_set_erase_char(struct open_file *file, uint8_t ch) {
  struct pty *pty = pty_for_file(file);
  if (pty != NULL && ch != 0) { pty->erase = ch; }
}

void cell_pty_get_winsize(const struct open_file *file, uint16_t *rows, uint16_t *cols) {
  struct pty *pty = pty_for_file(file);
  if (rows != NULL) { *rows = pty == NULL ? 38 : pty->rows; }
  if (cols != NULL) { *cols = pty == NULL ? 96 : pty->cols; }
}

void cell_pty_set_winsize(struct open_file *file, uint16_t rows, uint16_t cols) {
  struct pty *pty = pty_for_file(file);
  if (pty == NULL) { return; }
  if (rows != 0) { pty->rows = rows; }
  if (cols != 0) { pty->cols = cols; }
}

int cell_pty_foreground_pgrp(const struct open_file *file) {
  struct pty *pty = pty_for_file(file);
  if (pty == NULL) { return -ENOTTY; }
  if (pty->foreground_pgrp <= 0) {
    struct domain *domain = cell_current_domain_internal();
    if (domain != NULL) { pty->foreground_pgrp = domain->pgrp_id; }
  }
  return pty->foreground_pgrp;
}

int cell_pty_set_foreground_pgrp(struct open_file *file, int pgid) {
  struct pty *pty = pty_for_file(file);
  if (pty == NULL) { return -ENOTTY; }
  if (pgid <= 0) { return -EINVAL; }
  pty->foreground_pgrp = pgid;
  return 0;
}

int64_t cell_pty_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (domain == NULL || file == NULL || file->type != OPEN_PTY) { return -9; }
  if (len == 0) { return 0; }
  struct pty *pty = pty_for_file(file);
  if (pty == NULL) { return -EIO; }
  struct pty_ring *ring = file->pty_master ? &pty->to_master : &pty->to_slave;
  uint16_t peer_refs = file->pty_master ? pty->slave_refs : pty->master_refs;
  if (ring->len == 0) { return peer_refs == 0 ? 0 : -EAGAIN; }
  uint64_t got = 0;
  while (got < len) {
    char c;
    if (!ring_pop(ring, &c)) { break; }
    if (!vmm_copy_to_user(cell_domain_as(domain), buf + got, &c, 1)) { return got == 0 ? -EFAULT : (int64_t)got; }
    ++got;
  }
  if (got != 0) { pty_notify(); }
  return (int64_t)got;
}

int64_t cell_pty_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (domain == NULL || file == NULL || file->type != OPEN_PTY) { return -9; }
  if (len == 0) { return 0; }
  struct pty *pty = pty_for_file(file);
  if (pty == NULL) { return -EIO; }
  struct pty_ring *ring = file->pty_master ? &pty->to_slave : &pty->to_master;
  uint16_t peer_refs = file->pty_master ? pty->slave_refs : pty->master_refs;
  if (peer_refs == 0) { return -EIO; }
  uint64_t wrote = 0;
  while (wrote < len) {
    char c;
    if (!vmm_copy_from_user(cell_domain_as(domain), &c, buf + wrote, 1)) {
      return wrote == 0 ? -EFAULT : (int64_t)wrote;
    }
    bool map_newline = !file->pty_master && c == '\n' && (pty->oflag & TTY_OPOST) != 0 && (pty->oflag & TTY_ONLCR) != 0;
    size_t needed = map_newline ? 2u : 1u;
    if (ring_room(ring) < needed) { break; }
    if (map_newline) { (void)ring_push(ring, '\r'); }
    (void)ring_push(ring, c);
    ++wrote;
  }
  if (wrote == 0) { return -EAGAIN; }
  if (wrote != 0) { pty_notify(); }
  return (int64_t)wrote;
}

static void clear_pty_wait(struct thread *thread) {
  thread->wait_target = -1;
  thread->pipe_buf = 0;
  thread->pipe_len = 0;
  thread->pipe_write = false;
}

static void wake_pty_waiters(void) {
  if (pty_waking) { return; }
  pty_waking = true;
  for (size_t i = 0; i < cell_thread_capacity(); ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_PTY ||
        thread->domain == NULL) {
      continue;
    }
    int fd = thread->wait_target;
    if (fd < 0 || fd >= MAX_FDS || thread->domain->fds[fd] == NULL) {
      thread->tf.x[0] = (uint64_t)-9;
    } else {
      struct open_file *file = thread->domain->fds[fd];
      int64_t rc = thread->pipe_write
                     ? cell_pty_write_from_domain(thread->domain, file, thread->pipe_buf, thread->pipe_len)
                     : cell_pty_read_to_domain(thread->domain, file, thread->pipe_buf, thread->pipe_len);
      if (rc == -EAGAIN) { continue; }
      thread->tf.x[0] = (uint64_t)rc;
    }
    thread->state = THREAD_RUNNABLE;
    thread->wait_reason = WAIT_NONE;
    clear_pty_wait(thread);
  }
  pty_waking = false;
  cell_wake_poll_waiters_internal();
}

int cell_block_current_on_pty(int fd, uint64_t buf, uint64_t len, bool write, struct trap_frame *frame) {
  struct thread *thread = cell_current_thread_internal();
  cell_save_current(frame);
  thread->running_cpu = -1;
  thread->state = THREAD_BLOCKED;
  thread->wait_reason = WAIT_PTY;
  thread->wait_target = fd;
  thread->pipe_buf = buf;
  thread->pipe_len = len;
  thread->pipe_write = write;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

static void pty_notify(void) {
  wake_pty_waiters();
}
