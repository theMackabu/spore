#include "proc/fd.h"

#include "kstr.h"
#include "mem.h"
#include "proc/domain.h"
#include "proc/pipe.h"
#include "proc/poll.h"
#include "proc/socket.h"

enum {
  CELL_O_ACCMODE = 3,
  CELL_O_WRONLY = 1,
  CELL_O_NONBLOCK = 04000,
  CELL_O_APPEND = 02000,
  CELL_O_CLOEXEC = 02000000,
  EFD_SEMAPHORE = 1,
  EAGAIN = 11,
  EFAULT = 14,
  EINVAL = 22,
  ENXIO = 6,
  CELL_S_IFMT = 0170000,
  CELL_S_IFIFO = 0010000,
};

static struct open_file open_files[MAX_OPEN_FILES];

void cell_fd_table_reset(void) {
  kmemset(open_files, 0, sizeof(open_files));
}

int cell_find_free_fd(struct domain *domain, int start) {
  if (domain == NULL) { return -1; }
  for (int fd = start; fd < MAX_FDS; ++fd) {
    if (domain->fds[fd] == NULL) { return fd; }
  }
  return -1;
}

struct open_file *cell_alloc_open_file(void) {
  for (size_t i = 0; i < MAX_OPEN_FILES; ++i) {
    if (!open_files[i].used) {
      kmemset(&open_files[i], 0, sizeof(open_files[i]));
      open_files[i].used = true;
      open_files[i].refcount = 1;
      return &open_files[i];
    }
  }
  return NULL;
}

void cell_retain_open_file(struct open_file *file) {
  if (file != NULL) { ++file->refcount; }
}

void cell_release_open_file(struct open_file *file) {
  if (file == NULL || file->refcount == 0) { return; }
  --file->refcount;
  if (file->refcount == 0) {
    if (cell_pipe_release_file(file)) {
      cell_pipe_notify();
    } else if (file->type == OPEN_UNIX_STREAM) {
      cell_pipe_drop_reader(file->unix_rx_pipe);
      cell_pipe_drop_writer(file->unix_tx_pipe);
      cell_pipe_notify();
      cell_socket_wake_file(file);
    } else if (file->type == OPEN_UNIX_LISTENER) {
      cell_socket_release_listener(file);
    }
    file->used = false;
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

int64_t cell_eventfd_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (len < sizeof(uint64_t)) { return -EINVAL; }
  uint64_t add = 0;
  if (!vmm_copy_from_user(&domain->as, &add, buf, sizeof(add))) { return -EFAULT; }
  if (add == UINT64_MAX || UINT64_MAX - file->eventfd_value <= add) { return -EAGAIN; }
  file->eventfd_value += add;
  cell_wake_poll_waiters_internal();
  return sizeof(add);
}

int64_t cell_eventfd_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (len < sizeof(uint64_t)) { return -EINVAL; }
  if (file->eventfd_value == 0) { return -EAGAIN; }
  uint64_t value = file->eventfd_semaphore ? 1 : file->eventfd_value;
  file->eventfd_value -= value;
  if (!vmm_copy_to_user(&domain->as, buf, &value, sizeof(value))) { return -EFAULT; }
  cell_wake_poll_waiters_internal();
  return sizeof(value);
}

int cell_fd_eventfd(uint64_t initval, int flags) {
  if ((flags & ~(CELL_O_CLOEXEC | CELL_O_NONBLOCK | EFD_SEMAPHORE)) != 0) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -24; }
  struct open_file *file = cell_alloc_open_file();
  if (file == NULL) { return -12; }
  file->type = OPEN_EVENTFD;
  file->flags = (uint32_t)(flags & ~CELL_O_CLOEXEC);
  file->eventfd_value = initval;
  file->eventfd_semaphore = (flags & EFD_SEMAPHORE) != 0;
  domain->fds[fd] = file;
  domain->fd_flags[fd] = (flags & CELL_O_CLOEXEC) != 0 ? 1 : 0;
  return fd;
}

int64_t cell_fd_pread_kernel(int fd, uint64_t off, void *buf, uint64_t len) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_RAMFS || file->node.is_dir || file->node.device != RAMFS_DEV_NONE) { return -22; }
  return (int64_t)vfs_read(&file->node, off, buf, len);
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
  if ((node->mode & CELL_S_IFMT) == CELL_S_IFIFO) {
    int pipe_id = cell_fifo_pipe_for_ino(node->ino, true);
    if (pipe_id < 0) { return -23; }
    bool write_end = (flags & CELL_O_ACCMODE) == CELL_O_WRONLY;
    if ((flags & CELL_O_NONBLOCK) != 0 && write_end && !cell_pipe_has_readers((uint8_t)pipe_id)) { return -ENXIO; }

    int fd = cell_find_free_fd(domain, 0);
    if (fd < 0) { return -24; }
    struct open_file *file = cell_alloc_open_file();
    if (file == NULL) { return -12; }
    file->type = OPEN_PIPE;
    file->flags = flags & ~(uint32_t)CELL_O_CLOEXEC;
    file->node = *node;
    cell_copy_open_path(file, path);
    file->pipe_id = (uint8_t)pipe_id;
    file->pipe_write_end = write_end;
    if (write_end) {
      cell_pipe_add_writer((uint8_t)pipe_id);
    } else {
      cell_pipe_add_reader((uint8_t)pipe_id);
    }
    domain->fds[fd] = file;
    domain->fd_flags[fd] = (flags & CELL_O_CLOEXEC) != 0 ? 1 : 0;
    cell_pipe_notify();
    return fd;
  }
  int fd = -1;
  for (int i = 0; i < MAX_FDS; ++i) {
    if (domain->fds[i] == NULL) {
      fd = i;
      break;
    }
  }
  if (fd < 0) { return -24; }
  struct open_file *file = cell_alloc_open_file();
  if (file == NULL) { return -12; }
  file->type = OPEN_RAMFS;
  file->flags = flags & ~(uint32_t)CELL_O_CLOEXEC;
  file->node = *node;
  cell_copy_open_path(file, path);
  if ((flags & CELL_O_APPEND) != 0) { file->offset = node->size; }
  domain->fds[fd] = file;
  domain->fd_flags[fd] = (flags & CELL_O_CLOEXEC) != 0 ? 1 : 0;
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
  if (!vmm_copy_to_user(&domain->as, pipefd_addr, fds, sizeof(fds))) {
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

int cell_fd_get_flags(int fd) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  return (int)domain->fds[fd]->flags;
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
  if (file->type == OPEN_RAMFS && (file->node.device == RAMFS_DEV_CONSOLE || file->node.device == RAMFS_DEV_TTY)) {
    return 1;
  }
  return 0;
}

bool cell_fd_stat(int fd, struct vfs_node *out) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_STDOUT || file->type == OPEN_STDIN) {
    *out = (struct vfs_node){
      .backend = VFS_RAMFS,
      .ino = 10,
      .is_dir = false,
      .device = RAMFS_DEV_TTY,
      .mode = 0020666u,
      .links_count = 1,
      .dev_id = 0x0005,
      .rdev = (5u << 8),
    };
    return true;
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
  return vfs_refresh(&file->node, out);
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

bool cell_fd_next_dirent(int fd, struct vfs_dirent *out) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_RAMFS || !file->node.is_dir) { return false; }
  return vfs_next_dirent(&file->node, &file->offset, out);
}

void cell_fd_rewind_one_dirent(int fd) {
  struct domain *domain = cell_current_domain_internal();
  if (domain != NULL && fd >= 0 && fd < MAX_FDS && domain->fds[fd] != NULL && domain->fds[fd]->offset > 0) {
    --domain->fds[fd]->offset;
  }
}

uint64_t cell_fd_dir_offset(int fd) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return 0; }
  return domain->fds[fd]->offset;
}

void cell_fd_set_dir_offset(int fd, uint64_t offset) {
  struct domain *domain = cell_current_domain_internal();
  if (domain != NULL && fd >= 0 && fd < MAX_FDS && domain->fds[fd] != NULL) { domain->fds[fd]->offset = offset; }
}
