#include "flock.h"

#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/thread.h"

enum { EINVAL = 22, EWOULDBLOCK = 11 };

enum {
  LOCK_SH = 1,
  LOCK_EX = 2,
  LOCK_NB = 4,
  LOCK_UN = 8,
};

static struct open_file *locked_files;

static bool flock_same_file(const struct open_file *a, const struct open_file *b) {
  if (a->type != b->type) { return false; }
  if (a->type == OPEN_VFS) {
    return a->node.backend == b->node.backend && a->node.dev_id == b->node.dev_id && a->node.ino == b->node.ino;
  }
  if (a->type == OPEN_PIPE) { return a->pipe_id == b->pipe_id; }
  if (a->type == OPEN_PTY) { return a->pty_id == b->pty_id && a->pty_master == b->pty_master; }
  return false;
}

static bool flock_can_lock(const struct open_file *file, uint8_t mode) {
  for (const struct open_file *other = locked_files; other != NULL; other = other->flock_next) {
    if (other == file || !flock_same_file(file, other)) { continue; }
    if (mode == LOCK_EX || other->flock_mode == LOCK_EX) { return false; }
  }
  return true;
}

static void grant_lock(struct open_file *file, uint8_t mode) {
  if (file->flock_mode == 0) {
    file->flock_next = locked_files;
    locked_files = file;
  }
  file->flock_mode = mode;
}

static void wake_flock_waiters(void) {
  static bool waking;
  static bool rescan;
  if (waking) {
    rescan = true;
    return;
  }
  waking = true;
  do {
    rescan = false;
    size_t cursor = 0;
    struct thread *thread;
    while ((thread = cell_next_waiter(WAIT_FLOCK, &cursor))) {
      struct open_file *file = thread->wait.held_file;
      uint8_t mode = thread->wait.flock_mode;
      if (!flock_can_lock(file, mode)) { continue; }
      grant_lock(file, mode);
      cell_finish_wait(thread, 0);
    }
  } while (rescan);
  waking = false;
}

void cell_flock_release(struct open_file *file) {
  if (file->flock_mode == 0) { return; }
  struct open_file **link = &locked_files;
  while (*link != file) { link = &(*link)->flock_next; }
  *link = file->flock_next;
  file->flock_next = NULL;
  file->flock_mode = 0;
  wake_flock_waiters();
}

int cell_fd_flock(int fd, int operation, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  int mode = operation & (LOCK_SH | LOCK_EX | LOCK_UN);
  if ((operation & ~(LOCK_SH | LOCK_EX | LOCK_NB | LOCK_UN)) != 0 || mode == 0 || (mode & (mode - 1)) != 0) {
    return -EINVAL;
  }
  if (file->flock_mode == mode) { return 0; }
  cell_flock_release(file);
  if (mode == LOCK_UN) { return 0; }
  if (flock_can_lock(file, (uint8_t)mode)) {
    grant_lock(file, (uint8_t)mode);
    return 0;
  }
  if ((operation & LOCK_NB) != 0) { return -EWOULDBLOCK; }
  cell_retain_open_file(file);
  return cell_block_current(frame, (struct thread_wait){.reason = WAIT_FLOCK,
                                                        .target = -1,
                                                        .held_file = file,
                                                        .flock_mode = (uint8_t)mode});
}
