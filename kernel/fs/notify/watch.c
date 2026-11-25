#include "fs/vfs.h"
#include "internal.h"
#include "mm/pool.h"
#include "proc/domain.h"
#include "proc/fd.h"

enum {
  NONBLOCK = 04000,
  CLOEXEC = 02000000,
  ALL_EVENTS = 0xfff,
  EXCL_UNLINK = 1U << 26,
  MASK_CREATE = 1U << 28,
  MASK_ADD = 1U << 29,
  ONESHOT = 1U << 31,
  WATCH_BUCKETS = 128,
};

struct inotify_watch {
  struct inotify_state *state;
  struct inotify_watch *next, **link, *inode_next, **inode_link;
  uint64_t dev, ino;
  uint32_t mask;
  int wd;
  bool directory;
};

static struct object_pool instances, watches;
static struct inotify_watch *by_inode[WATCH_BUCKETS];
static unsigned watch_count, directory_watches;

static struct inotify_watch **bucket(uint64_t dev, uint64_t ino) {
  return &by_inode[(dev ^ ino) % WATCH_BUCKETS];
}

int inotify_open(int flags) {
  if (flags & ~(NONBLOCK | CLOEXEC)) { return -22; }
  if (!instances.limit && !pool_init(&instances, sizeof(struct inotify_state), SIZE_MAX)) { return -12; }
  struct inotify_state *state = pool_alloc(&instances);
  if (!state) { return -12; }
  struct open_file *file = cell_alloc_open_file();
  if (!file) {
    pool_free(&instances, state);
    return -12;
  }
  state->file = file;
  file->type = OPEN_INOTIFY;
  file->flags = flags & NONBLOCK;
  file->inotify = state;
  return cell_install_fd(cell_current_domain_internal(), file, flags & CLOEXEC);
}

static struct inotify_watch *find_wd(struct inotify_state *state, int wd) {
  for (struct inotify_watch *w = state->watches; w; w = w->next) {
    if (w->wd == wd) { return w; }
  }
  return NULL;
}

int inotify_add(int fd, const struct vfs_node *node, uint32_t mask) {
  struct open_file *file = cell_fd_file(fd);
  if (!file) { return -9; }
  if (file->type != OPEN_INOTIFY || !(mask & ALL_EVENTS) || ((mask & MASK_ADD) && (mask & MASK_CREATE)) ||
      (mask & ~(ALL_EVENTS | FS_ONLYDIR | FS_DONT_FOLLOW | EXCL_UNLINK | MASK_CREATE | MASK_ADD | ONESHOT))) {
    return -22;
  }
  if ((mask & FS_ONLYDIR) && !node->is_dir) { return -20; }
  struct inotify_state *state = file->inotify;
  struct inotify_watch **head = bucket(node->dev_id, node->ino);
  for (struct inotify_watch *w = *head; w; w = w->inode_next) {
    if (w->state != state || w->dev != node->dev_id || w->ino != node->ino) { continue; }
    if (mask & MASK_CREATE) { return -17; }
    w->mask = (mask & MASK_ADD ? w->mask : 0) | mask;
    return w->wd;
  }
  if (!watches.limit && !pool_init(&watches, sizeof(struct inotify_watch), SIZE_MAX)) { return -28; }
  struct inotify_watch *w = pool_alloc(&watches);
  if (!w) { return -28; }
  do {
    if (state->next_wd == INT32_MAX) {
      state->next_wd = 0;
      state->wd_wrapped = true;
    }
    ++state->next_wd;
  } while (state->wd_wrapped && find_wd(state, (int)state->next_wd));
  w->wd = (int)state->next_wd;
  w->dev = node->dev_id;
  w->ino = node->ino;
  w->directory = node->is_dir;
  w->mask = mask;
  w->state = state;
  w->link = &state->watches;
  w->next = state->watches;
  if (w->next) { w->next->link = &w->next; }
  state->watches = w;
  w->inode_link = head;
  w->inode_next = *head;
  if (w->inode_next) { w->inode_next->inode_link = &w->inode_next; }
  *head = w;
  ++watch_count;
  directory_watches += w->directory;
  return w->wd;
}

static void remove_watch(struct inotify_watch *w, bool report) {
  if (report) { inotify_enqueue(w->state, w->wd, FS_IGNORED, 0, NULL); }
  *w->inode_link = w->inode_next;
  if (w->inode_next) { w->inode_next->inode_link = w->inode_link; }
  *w->link = w->next;
  if (w->next) { w->next->link = w->link; }
  --watch_count;
  directory_watches -= w->directory;
  pool_free(&watches, w);
}

int inotify_remove(int fd, int wd) {
  struct open_file *file = cell_fd_file(fd);
  if (!file) { return -9; }
  if (file->type != OPEN_INOTIFY) { return -22; }
  struct inotify_watch *w = find_wd(file->inotify, wd);
  if (!w) { return -22; }
  remove_watch(w, true);
  inotify_flush();
  return 0;
}

void inotify_close(struct open_file *file) {
  struct inotify_state *state = file->inotify;
  while (state->watches) {
    remove_watch(state->watches, false);
  }
  inotify_clear_events(state);
  pool_free(&instances, state);
}

bool fsnotify_directories(void) {
  return directory_watches != 0;
}

bool fsnotify_active(void) {
  return watch_count != 0;
}

void fsnotify_emit(uint64_t dev, uint64_t ino, uint32_t mask, uint32_t cookie, const char *name, bool unlinked) {
  struct inotify_watch *w = *bucket(dev, ino);
  while (w) {
    struct inotify_watch *next = w->inode_next;
    if (w->dev == dev && w->ino == ino && (w->mask & mask & ALL_EVENTS) && !(unlinked && (w->mask & EXCL_UNLINK))) {
      inotify_enqueue(w->state, w->wd, mask, cookie, name);
      if (w->mask & ONESHOT) { remove_watch(w, true); }
    }
    w = next;
  }
}

bool fsnotify_hold_watched(const char *path, struct vfs_node *node) {
  if (!watch_count || !vfs_lstat(path, node) || node->is_dir) { return false; }
  for (struct inotify_watch *w = *bucket(node->dev_id, node->ino); w; w = w->inode_next) {
    if (w->dev == node->dev_id && w->ino == node->ino) { return vfs_retain_node(node); }
  }
  return false;
}

void fsnotify_reclaim(uint64_t dev, uint64_t ino) {
  struct inotify_watch *w = *bucket(dev, ino);
  while (w) {
    struct inotify_watch *next = w->inode_next;
    if (w->dev == dev && w->ino == ino) {
      if (w->mask & FS_DELETE_SELF) { inotify_enqueue(w->state, w->wd, FS_DELETE_SELF, 0, NULL); }
      remove_watch(w, true);
    }
    w = next;
  }
}
