#include "abi/linux/aarch64/open.h"
#include "proc/fd.h"
#include "flock.h"

#include "lib/kstr.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "mm/pool.h"
#include "proc/domain.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/pipe.h"
#include "proc/poll.h"
#include "proc/thread.h"
#include "tty/pty.h"
#include "net/socket.h"

enum {
  CELL_O_NONBLOCK = 04000,
  CELL_O_APPEND = 02000,
  CELL_O_CLOEXEC = 02000000,
  IN_NONBLOCK = 04000,
  IN_CLOEXEC = 02000000,
  ENOMEM = 12,
  EFAULT = 14,
  EINVAL = 22,
  ENOSPC = 28,
  ENXIO = 6,
  CELL_S_IFMT = 0170000,
  CELL_S_IFIFO = 0010000,
};

struct inotify_watch {
  int32_t wd;
  uint32_t mask;
  char path[CELL_PATH_MAX];
};

struct inotify_state {
  uint32_t next_wd;
  struct inotify_watch watches[16];
};

constexpr size_t INOTIFY_PAGES = (sizeof(struct inotify_state) + PAGE_SIZE - 1) / PAGE_SIZE;

static void free_inotify_pages(uint64_t pa) {
  for (size_t i = 0; i < INOTIFY_PAGES; ++i) {
    pmm_free_page(pa + i * PAGE_SIZE);
  }
}

static struct object_pool open_files;

bool cell_fd_table_reset(size_t capacity) {
  return capacity <= MAX_OPEN_FILES && pool_init(&open_files, sizeof(struct open_file), capacity);
}

int cell_find_free_fd(struct domain *domain, int start) {
  if (domain == NULL) { return -1; }
  for (int fd = start; fd < MAX_FDS; ++fd) {
    if (domain->fds[fd] == NULL) { return fd; }
  }
  return -1;
}

struct open_file *cell_alloc_open_file(void) {
  struct open_file *file = pool_alloc(&open_files);
  if (file != NULL) { file->refcount = 1; }
  return file;
}

void cell_retain_open_file(struct open_file *file) {
  if (file != NULL) { ++file->refcount; }
}

void cell_release_open_file(struct open_file *file) {
  if (file == NULL || file->refcount == 0) { return; }
  --file->refcount;
  if (file->refcount == 0) {
    cell_flock_release(file);
    if (cell_pipe_release_file(file)) {
      cell_pipe_notify();
    } else if (file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER) {
      cell_unix_release_file(file);
    } else if (file->type == OPEN_SOCKET) {
      cell_socket_release_file(file);
    } else if (file->type == OPEN_INOTIFY && file->inotify_pa != 0) {
      free_inotify_pages(file->inotify_pa);
      file->inotify_pa = 0;
    } else if (file->type == OPEN_PTY) {
      cell_pty_release_file(file);
    }
    if (file->type == OPEN_VFS || file->type == OPEN_PIPE) { vfs_release_node(&file->node); }
    pool_free(&open_files, file);
  }
}

void cell_close_all_fds(struct domain *domain) {
  for (size_t i = 0; i < MAX_FDS; ++i) {
    cell_release_open_file(domain->fds[i]);
    domain->fds[i] = NULL;
    domain->fd_flags[i] = 0;
  }
}

bool cell_init_stdio(struct domain *domain) {
  struct open_file *in = cell_alloc_open_file();
  struct open_file *out = cell_alloc_open_file();
  struct open_file *err = cell_alloc_open_file();
  if (in == NULL || out == NULL || err == NULL) {
    cell_release_open_file(in);
    cell_release_open_file(out);
    cell_release_open_file(err);
    return false;
  }
  in->type = OPEN_STDIN;
  out->type = OPEN_STDOUT;
  err->type = OPEN_STDOUT;
  cell_copy_open_path(in, "/dev/console");
  cell_copy_open_path(out, "/dev/console");
  cell_copy_open_path(err, "/dev/console");
  domain->fds[0] = in;
  domain->fds[1] = out;
  domain->fds[2] = err;
  domain->fd_flags[0] = 0;
  domain->fd_flags[1] = 0;
  domain->fd_flags[2] = 0;
  return true;
}

void cell_copy_fd_table(struct domain *dst, const struct domain *src) {
  for (size_t i = 0; i < MAX_FDS; ++i) {
    dst->fds[i] = src->fds[i];
    dst->fd_flags[i] = src->fd_flags[i];
    cell_retain_open_file(dst->fds[i]);
  }
}

void cell_copy_open_path(struct open_file *file, const char *path) {
  if (file == NULL || path == NULL) { return; }
  size_t len = kstrlen(path);
  if (len >= sizeof(file->path)) { len = sizeof(file->path) - 1; }
  kmemcpy(file->path, path, len);
  file->path[len] = '\0';
}

int cell_fd_inotify_init1(int flags) {
  if ((flags & ~(IN_CLOEXEC | IN_NONBLOCK)) != 0) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -24; }
  struct open_file *file = cell_alloc_open_file();
  if (file == NULL) { return -12; }
  file->type = OPEN_INOTIFY;
  file->flags = (uint32_t)(flags & ~IN_CLOEXEC);
  domain->fds[fd] = file;
  domain->fd_flags[fd] = (flags & IN_CLOEXEC) != 0 ? 1 : 0;
  return fd;
}

static struct inotify_state *ensure_inotify_state(struct open_file *file) {
  if (file->inotify_pa != 0) { return pmm_phys_to_virt(file->inotify_pa); }
  uint64_t pa = pmm_alloc_contiguous_pages(INOTIFY_PAGES);
  if (pa == 0) { return NULL; }
  struct inotify_state *state = pmm_phys_to_virt(pa);
  if (state == NULL) {
    free_inotify_pages(pa);
    return NULL;
  }
  kmemset(state, 0, sizeof(*state));
  state->next_wd = 1;
  file->inotify_pa = pa;
  return state;
}

int cell_fd_inotify_add_watch(int fd, const char *path, uint32_t mask) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || path == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL ||
      domain->fds[fd]->type != OPEN_INOTIFY) {
    return -9;
  }
  if (mask == 0) { return -EINVAL; }
  struct inotify_state *state = ensure_inotify_state(domain->fds[fd]);
  if (state == NULL) { return -ENOMEM; }
  struct inotify_watch *free_slot = NULL;
  for (size_t i = 0; i < sizeof(state->watches) / sizeof(state->watches[0]); ++i) {
    struct inotify_watch *watch = &state->watches[i];
    if (watch->wd != 0 && str_eq(watch->path, path)) {
      watch->mask = mask;
      return watch->wd;
    }
    if (watch->wd == 0 && free_slot == NULL) { free_slot = watch; }
  }
  if (free_slot == NULL) { return -ENOSPC; }
  uint32_t wd = state->next_wd++;
  if (wd == 0 || wd > INT32_MAX) {
    wd = 1;
    state->next_wd = 2;
  }
  free_slot->wd = (int32_t)wd;
  free_slot->mask = mask;
  copy_cstr(free_slot->path, sizeof(free_slot->path), path);
  return (int)wd;
}

int cell_fd_inotify_rm_watch(int fd, int wd) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL || domain->fds[fd]->type != OPEN_INOTIFY) {
    return -9;
  }
  uint64_t pa = domain->fds[fd]->inotify_pa;
  if (wd <= 0 || pa == 0) { return -EINVAL; }
  struct inotify_state *state = pmm_phys_to_virt(pa);
  for (size_t i = 0; i < sizeof(state->watches) / sizeof(state->watches[0]); ++i) {
    if (state->watches[i].wd == wd) {
      kmemset(&state->watches[i], 0, sizeof(state->watches[i]));
      return 0;
    }
  }
  return -EINVAL;
}

int64_t cell_fd_lseek(int fd, int64_t off, int whence) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  int64_t base = 0;
  if (whence == 0) {
    base = 0;
  } else if (whence == 1) {
    base = (int64_t)file->offset;
  } else if (whence == 2) {
    struct vfs_node fresh;
    base = vfs_refresh(&file->node, &fresh) ? (int64_t)fresh.size : 0;
  } else {
    return -22;
  }
  int64_t next = base + off;
  if (next < 0) { return -22; }
  file->offset = (uint64_t)next;
  return next;
}

int cell_fd_open_node(const struct vfs_node *node, uint32_t flags, const char *path) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  if (node->device == RAMFS_DEV_PTMX) { return cell_pty_open_master(flags, path); }
  if (node->device == RAMFS_DEV_PTS) {
    int id = cell_pty_id_from_path(path);
    if (id < 0) { return id; }
    return cell_pty_open_slave(id, flags, path);
  }
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -24; }
  struct open_file *file = cell_alloc_open_file();
  if (file == NULL) { return -12; }
  if (!vfs_retain_node(node)) {
    cell_release_open_file(file);
    return -ENOMEM;
  }
  file->type = OPEN_VFS;
  file->node = *node;
  if ((node->mode & CELL_S_IFMT) == CELL_S_IFIFO) {
    bool write_end = (flags & LINUX_O_ACCMODE) == LINUX_O_WRONLY;
    bool needs_reader = write_end && (flags & CELL_O_NONBLOCK) != 0;
    int pipe_id = cell_fifo_pipe_for_ino(node->ino, !needs_reader);
    if (needs_reader && (pipe_id < 0 || !cell_pipe_has_readers((uint8_t)pipe_id))) {
      cell_release_open_file(file);
      return -ENXIO;
    }
    if (pipe_id < 0) {
      cell_release_open_file(file);
      return -23;
    }
    file->type = OPEN_PIPE;
    file->pipe_id = (uint8_t)pipe_id;
    file->pipe_write_end = write_end;
    if (write_end) {
      cell_pipe_add_writer((uint8_t)pipe_id);
    } else {
      cell_pipe_add_reader((uint8_t)pipe_id);
    }
  }
  file->flags = flags & ~(uint32_t)CELL_O_CLOEXEC;
  cell_copy_open_path(file, path);
  if (file->type == OPEN_VFS && (flags & CELL_O_APPEND) != 0) { file->offset = node->size; }
  domain->fds[fd] = file;
  domain->fd_flags[fd] = (flags & CELL_O_CLOEXEC) != 0 ? 1 : 0;
  if (file->type == OPEN_PIPE) { cell_pipe_notify(); }
  return fd;
}

int cell_fd_pipe2(uint64_t pipefd_addr, int flags) {
  if ((flags & ~(CELL_O_CLOEXEC | CELL_O_NONBLOCK)) != 0) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  int read_fd = cell_find_free_fd(domain, 0);
  int write_fd = read_fd < 0 ? -1 : cell_find_free_fd(domain, read_fd + 1);
  if (read_fd < 0 || write_fd < 0) { return -24; }

  int pipe_id = cell_alloc_pipe_obj(0);
  if (pipe_id < 0) { return -23; }

  struct open_file *read_file = cell_alloc_open_file();
  struct open_file *write_file = cell_alloc_open_file();
  if (read_file == NULL || write_file == NULL) {
    cell_release_open_file(read_file);
    cell_release_open_file(write_file);
    cell_pipe_free((uint8_t)pipe_id);
    return -12;
  }

  cell_pipe_add_reader((uint8_t)pipe_id);
  cell_pipe_add_writer((uint8_t)pipe_id);

  read_file->type = OPEN_PIPE;
  read_file->flags = (uint32_t)(flags & ~CELL_O_CLOEXEC);
  read_file->pipe_id = (uint8_t)pipe_id;
  read_file->pipe_write_end = false;
  write_file->type = OPEN_PIPE;
  write_file->flags = (uint32_t)(flags & ~CELL_O_CLOEXEC);
  write_file->pipe_id = (uint8_t)pipe_id;
  write_file->pipe_write_end = true;

  int fds[2] = {read_fd, write_fd};
  if (!vmm_copy_to_user(cell_domain_as(domain), pipefd_addr, fds, sizeof(fds))) {
    cell_release_open_file(read_file);
    cell_release_open_file(write_file);
    return -EFAULT;
  }
  domain->fds[read_fd] = read_file;
  domain->fds[write_fd] = write_file;
  domain->fd_flags[read_fd] = (flags & CELL_O_CLOEXEC) != 0 ? 1 : 0;
  domain->fd_flags[write_fd] = (flags & CELL_O_CLOEXEC) != 0 ? 1 : 0;
  return 0;
}

int cell_fd_dup(int oldfd, int minfd) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || oldfd < 0 || oldfd >= MAX_FDS || minfd < 0 || minfd >= MAX_FDS || domain->fds[oldfd] == NULL) {
    return -9;
  }
  for (int fd = minfd; fd < MAX_FDS; ++fd) {
    if (domain->fds[fd] == NULL) {
      domain->fds[fd] = domain->fds[oldfd];
      domain->fd_flags[fd] = 0;
      cell_retain_open_file(domain->fds[fd]);
      return fd;
    }
  }
  return -24;
}

int cell_fd_dup3(int oldfd, int newfd, int flags) {
  if ((flags & ~CELL_O_CLOEXEC) != 0) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || oldfd < 0 || oldfd >= MAX_FDS || newfd < 0 || newfd >= MAX_FDS || domain->fds[oldfd] == NULL) {
    return -9;
  }
  if (oldfd == newfd) { return -EINVAL; }
  struct open_file *file = domain->fds[oldfd];
  cell_retain_open_file(file);
  if (domain->fds[newfd] != NULL) { cell_release_open_file(domain->fds[newfd]); }
  domain->fds[newfd] = file;
  domain->fd_flags[newfd] = (flags & CELL_O_CLOEXEC) != 0 ? 1 : 0;
  return newfd;
}

struct open_file *cell_fd_file(uint64_t fd) {
  struct domain *domain = cell_current_domain_internal();
  return domain && fd < MAX_FDS ? domain->fds[fd] : NULL;
}

int cell_fd_get_flags(int fd) {
  struct open_file *file = cell_fd_file(fd);
  return file ? (int)file->flags : -9;
}

int cell_fd_set_flags(int fd, int flags) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  uint32_t mutable = (uint32_t)(CELL_O_NONBLOCK | CELL_O_APPEND);
  uint32_t keep = domain->fds[fd]->flags & ~mutable;
  uint32_t status = (uint32_t)flags & mutable;
  domain->fds[fd]->flags = keep | status;
  return 0;
}

int cell_fd_get_fd_flags(int fd) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  return domain->fd_flags[fd] != 0 ? 1 : 0;
}

int cell_fd_set_fd_flags(int fd, int flags) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  domain->fd_flags[fd] = (flags & 1) != 0 ? 1 : 0;
  return 0;
}

int cell_fd_add_seals(int fd, uint32_t seals) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_VFS || file->node.device != RAMFS_DEV_NONE) { return -22; }
  if (!linux_open_writable(file->flags)) { return -1; }
  return vfs_add_seals(&file->node, seals);
}

int cell_fd_get_seals(int fd) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_VFS || file->node.device != RAMFS_DEV_NONE) { return -22; }
  return vfs_get_seals(&file->node);
}

int cell_fd_close(int fd) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  cell_release_open_file(domain->fds[fd]);
  domain->fds[fd] = NULL;
  domain->fd_flags[fd] = 0;
  return 0;
}

int cell_fd_is_tty(int fd) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_STDIN || file->type == OPEN_STDOUT) { return 1; }
  if (file->type == OPEN_VFS && (file->node.device == RAMFS_DEV_CONSOLE || file->node.device == RAMFS_DEV_TTY)) {
    return 1;
  }
  if (file->type == OPEN_PTY) { return 1; }
  return 0;
}

bool cell_fd_stat(int fd, struct vfs_node *out) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_STDOUT || file->type == OPEN_STDIN) {
    return vfs_lookup("/dev/console", out);
  }
  if (file->type == OPEN_PIPE) {
    *out = (struct vfs_node){
      .backend = VFS_RAMFS,
      .ino = 20 + (uint64_t)file->pipe_id,
      .is_dir = false,
      .device = RAMFS_DEV_NONE,
      .mode = 0010666u,
      .links_count = 1,
      .dev_id = 0x0005,
    };
    return true;
  }
  if (file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER) {
    *out = file->node;
    if (out->mode == 0) {
      out->backend = VFS_RAMFS;
      out->mode = 0140666u;
      out->links_count = 1;
      out->dev_id = 0x0012;
    }
    return true;
  }
  if (file->type == OPEN_INOTIFY) {
    *out = (struct vfs_node){
      .backend = VFS_RAMFS,
      .ino = 30,
      .is_dir = false,
      .device = RAMFS_DEV_NONE,
      .mode = 0100600u,
      .links_count = 1,
      .dev_id = 0x0005,
    };
    return true;
  }
  if (file->type == OPEN_PTY) {
    uint64_t minor = file->pty_master ? 2u : file->pty_id;
    *out = (struct vfs_node){
      .backend = VFS_RAMFS,
      .ino = 100 + (uint64_t)file->pty_id + (file->pty_master ? 0 : 1000),
      .is_dir = false,
      .device = file->pty_master ? RAMFS_DEV_PTMX : RAMFS_DEV_PTS,
      .mode = 0020666u,
      .links_count = 1,
      .dev_id = 0x0005,
      .rdev = ((uint64_t)(file->pty_master ? 5u : 136u) << 8) | minor,
    };
    return true;
  }
  return vfs_refresh(&file->node, out);
}

int cell_fd_get_dir_node(int fd, struct vfs_node *out, uint64_t *cursor) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_VFS || !file->node.is_dir) { return -20; }
  *out = file->node;
  *cursor = file->offset;
  return 0;
}

bool cell_fd_is_dir(int fd) {
  struct vfs_node node;
  return cell_fd_stat(fd, &node) && node.is_dir;
}

bool cell_fd_path(int fd, char *out, size_t cap) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || out == NULL || cap == 0 || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->path[0] == '\0') { return false; }
  size_t len = kstrlen(file->path);
  if (len >= cap) { return false; }
  kmemcpy(out, file->path, len + 1);
  return true;
}

void cell_fd_set_dir_offset(int fd, uint64_t offset) {
  struct domain *domain = cell_current_domain_internal();
  if (domain != NULL && fd >= 0 && fd < MAX_FDS && domain->fds[fd] != NULL) { domain->fds[fd]->offset = offset; }
}
