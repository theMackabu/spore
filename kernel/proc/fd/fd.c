#include "proc/fd.h"
#include "abi/linux/shared/open.h"
#include "flock.h"
#include "proc/epoll.h"
#include "proc/signalfd.h"

#include "fs/notify.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "mm/pool.h"
#include "net/socket.h"
#include "net/unix.h"
#include "proc/domain.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/pipe.h"
#include "proc/poll.h"
#include "proc/thread.h"
#include "tty/pty.h"

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

static struct object_pool open_files;

bool cell_fd_table_reset(size_t capacity) {
  return capacity <= MAX_OPEN_FILES && pool_init(&open_files, sizeof(struct open_file), capacity);
}

struct open_file *cell_alloc_open_file(void) {
  struct open_file *file = pool_alloc(&open_files);
  if (!file && unix_collect_cycles()) { file = pool_alloc(&open_files); }
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
    if (file->type == OPEN_SIGNALFD) { cell_signalfd_close(); }
    if (file->type == OPEN_URING) { uring_release(&file->ring); }
    if (file->type == OPEN_VFS || file->type == OPEN_PIPE) { fsnotify_closed(file); }
    if (file->epoll_observers || file->type == OPEN_EPOLL) { cell_epoll_release_file(file); }
    cell_flock_release(file);
    if (cell_pipe_release_file(file)) {
      cell_pipe_notify();
    } else if (file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER) {
      cell_unix_release_file(file);
    } else if (file->type == OPEN_SOCKET) {
      cell_socket_release_file(file);
    } else if (file->type == OPEN_INOTIFY) {
      inotify_close(file);
    } else if (file->type == OPEN_PTY) {
      cell_pty_release_file(file);
      if (file->node.backend) { vfs_release_node(&file->node); }
    }
    if (file->type == OPEN_VFS && file->node.access && (file->node.mode & 0170000) == 0100000 &&
        linux_open_writable(file->flags)) {
      --file->node.access->writers;
    }
    if (file->type == OPEN_VFS || file->type == OPEN_PIPE) { vfs_release_node(&file->node); }
    pool_free(&open_files, file);
  } else if (file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER) {
    unix_check_last_reference(file);
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
  out->flags = LINUX_O_WRONLY;
  err->flags = LINUX_O_WRONLY;
  cell_copy_open_path(in, "/dev/console");
  cell_copy_open_path(out, "/dev/console");
  cell_copy_open_path(err, "/dev/console");
  cell_assign_fd(domain, 0, in, false);
  cell_assign_fd(domain, 1, out, false);
  cell_assign_fd(domain, 2, err, false);
  return true;
}

void cell_copy_open_path(struct open_file *file, const char *path) {
  if (file == NULL || path == NULL) { return; }
  size_t len = kstrlen(path);
  if (len >= sizeof(file->path)) { len = sizeof(file->path) - 1; }
  kmemcpy(file->path, path, len);
  file->path[len] = '\0';
}

int64_t cell_fd_lseek(int fd, int64_t off, int whence) {
  struct open_file *file = cell_fd_file(fd);
  if (!file || (file->flags & LINUX_O_PATH)) { return -9; }
  if (file->type != OPEN_VFS || file->node.device == RAMFS_DEV_TTY || file->node.device == RAMFS_DEV_CONSOLE) {
    return -29;
  }
  int64_t base = 0;
  switch (whence) {
  case 0:
    break;
  case 1:
    base = (int64_t)file->offset;
    break;
  case 2: {
    struct vfs_node fresh;
    if (!vfs_refresh(&file->node, &fresh)) { return -5; }
    base = (int64_t)fresh.size;
    break;
  }
  case 3:
  case 4:
    if (off < 0) { return -6; }
    off = vfs_seek_extent(&file->node, off, whence == 3);
    if (off < 0) { return off; }
    break;
  default:
    return -EINVAL;
  }
  int64_t next;
  if (__builtin_add_overflow(base, off, &next) || next < 0) { return -EINVAL; }
  if (file->node.device == RAMFS_DEV_NULL || file->node.device == RAMFS_DEV_ZERO ||
      file->node.device == RAMFS_DEV_FULL || file->node.device == RAMFS_DEV_URANDOM ||
      file->node.device == RAMFS_DEV_RANDOM) {
    next = 0;
  }
  file->offset = (uint64_t)next;
  return next;
}

int cell_fd_open_node(const struct vfs_node *node, uint32_t flags, const char *path, const char *resolved,
                      struct trap_frame *frame) {
  uint16_t type = node->mode & 0170000;
  if (!(flags & LINUX_O_PATH) && (type == 0020000 || type == 0060000) && vfs_mount_flag(node->access, MOUNT_NODEV)) {
    return -13;
  }
  if (linux_open_writable(flags) && vfs_mount_flag(node->access, MOUNT_RDONLY) && type == 0100000) { return -30; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  bool path_only = (flags & LINUX_O_PATH) != 0;
  if (!path_only && (node->mode & CELL_S_IFMT) == 0140000u) { return -ENXIO; }
  if (!path_only && (node->device == RAMFS_DEV_PTMX || node->device == RAMFS_DEV_PTS)) {
    int fd = cell_pty_open_node(&node->ramfs, flags, path);
    if (fd < 0) { return fd; }
    if (!vfs_retain_node(node)) {
      cell_close_domain_fd(domain, fd);
      return -12;
    }
    cell_file_at(domain, fd)->node = *node;
    return fd;
  }
  struct open_file *file = cell_alloc_open_file();
  if (file == NULL) { return -12; }
  if (!vfs_retain_node(node)) {
    cell_release_open_file(file);
    return -ENOMEM;
  }
  file->type = OPEN_VFS;
  file->node = *node;
  if (!path_only && (node->mode & CELL_S_IFMT) == CELL_S_IFIFO) {
    bool read = linux_open_readable(flags), write = linux_open_writable(flags);
    if (!read && !write) {
      cell_release_open_file(file);
      return -EINVAL;
    }
    bool needs_reader = write && !read && (flags & CELL_O_NONBLOCK) != 0;
    struct pipe_obj *pipe = cell_fifo_open(node, !needs_reader);
    if (needs_reader && (!pipe || !cell_pipe_has_readers(pipe))) {
      cell_release_open_file(file);
      return -ENXIO;
    }
    if (!pipe) {
      cell_release_open_file(file);
      return -23;
    }
    file->type = OPEN_PIPE;
    file->pipe = pipe;
    if (read) { cell_pipe_add_reader(pipe); }
    if (write) { cell_pipe_add_writer(pipe); }
  }
  if (file->type == OPEN_VFS && node->access && type == 0100000 && linux_open_writable(flags)) {
    ++node->access->writers;
  }
  file->flags = flags & ~(uint32_t)CELL_O_CLOEXEC;
  cell_copy_open_path(file, path);
  bool pipe = file->type == OPEN_PIPE;
  fsnotify_track_open(file, resolved);
  if (pipe && !(flags & CELL_O_NONBLOCK) && !cell_fifo_has_peer(file)) {
    if (!frame) {
      cell_release_open_file(file);
      return -EINVAL;
    }
    int slot = cell_reserve_fd(domain);
    if (slot < 0) {
      cell_release_open_file(file);
      return slot;
    }
    return cell_block_current(frame, (struct thread_wait){.reason = WAIT_FIFO_OPEN,
                                                          .target = slot,
                                                          .held_file = file,
                                                          .fifo_cloexec = (flags & CELL_O_CLOEXEC) != 0});
  }
  int fd = cell_install_fd(domain, file, flags & CELL_O_CLOEXEC);
  if (fd >= 0) {
    fsnotify_opened(file);
    if (pipe) { cell_pipe_notify(); }
  }
  return fd;
}

int cell_fd_pipe2(uint64_t pipefd_addr, int flags) {
  if ((flags & ~(CELL_O_CLOEXEC | CELL_O_NONBLOCK)) != 0) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  struct pipe_obj *pipe = cell_pipe_create();
  if (!pipe) { return -23; }

  struct open_file *read_file = cell_alloc_open_file();
  struct open_file *write_file = cell_alloc_open_file();
  if (read_file == NULL || write_file == NULL) {
    cell_release_open_file(read_file);
    cell_release_open_file(write_file);
    cell_pipe_discard(pipe);
    return -12;
  }

  cell_pipe_add_reader(pipe);
  cell_pipe_add_writer(pipe);

  read_file->type = OPEN_PIPE;
  read_file->flags = (uint32_t)(flags & ~CELL_O_CLOEXEC);
  read_file->pipe = pipe;
  write_file->type = OPEN_PIPE;
  write_file->flags = (uint32_t)(flags & ~CELL_O_CLOEXEC) | LINUX_O_WRONLY;
  write_file->pipe = pipe;

  return cell_install_fd_pair(domain, read_file, write_file, flags & CELL_O_CLOEXEC, pipefd_addr);
}

int cell_fd_get_flags(int fd) {
  struct open_file *file = cell_fd_file(fd);
  return file ? (int)file->flags : -9;
}

int cell_fd_set_flags(int fd, int flags) {
  struct domain *domain = cell_current_domain_internal();
  if (!cell_file_at(domain, fd) || (cell_file_at(domain, fd)->flags & LINUX_O_PATH)) { return -9; }
  uint32_t mutable = (uint32_t)(CELL_O_NONBLOCK | CELL_O_APPEND);
  uint32_t keep = cell_file_at(domain, fd)->flags & ~mutable;
  uint32_t status = (uint32_t)flags & mutable;
  cell_file_at(domain, fd)->flags = keep | status;
  return 0;
}

int cell_fd_add_seals(int fd, uint32_t seals) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = cell_file_at(domain, fd);
  if (file == NULL) { return -9; }
  if (file->type != OPEN_VFS || file->node.device != RAMFS_DEV_NONE) { return -22; }
  if (!linux_open_writable(file->flags)) { return -1; }
  return vfs_add_seals(&file->node, seals);
}

int cell_fd_get_seals(int fd) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = cell_file_at(domain, fd);
  if (file == NULL) { return -9; }
  if (file->type != OPEN_VFS || file->node.device != RAMFS_DEV_NONE) { return -22; }
  return vfs_get_seals(&file->node);
}

int cell_fd_is_tty(int fd) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = cell_file_at(domain, fd);
  if (file == NULL) { return -9; }
  if (file->type == OPEN_STDIN || file->type == OPEN_STDOUT) { return 1; }
  if (file->type == OPEN_VFS && (file->node.device == RAMFS_DEV_CONSOLE || file->node.device == RAMFS_DEV_TTY)) {
    return 1;
  }
  if (file->type == OPEN_PTY) { return 1; }
  return 0;
}

bool cell_fd_stat(int fd, struct vfs_node *out) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = cell_file_at(domain, fd);
  if (file == NULL) { return false; }
  if (file->type == OPEN_STDOUT || file->type == OPEN_STDIN) { return vfs_lookup("/dev/console", out); }
  if (file->type == OPEN_PIPE) {
    if (file->node.backend != VFS_NONE) { return vfs_refresh(&file->node, out); }
    *out = (struct vfs_node){
      .backend = VFS_RAMFS,
      .ino = cell_pipe_ino(file->pipe),
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
    if (file->node.backend) { return vfs_refresh(&file->node, out); }
    uint64_t minor = file->pty_master ? 2u : (unsigned)cell_pty_id(file);
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
  struct open_file *file = cell_file_at(domain, fd);
  if (!file || (file->flags & LINUX_O_PATH)) { return -9; }
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
  struct open_file *file = cell_file_at(domain, fd);
  if (domain == NULL || out == NULL || cap == 0 || file == NULL) { return false; }
  if (file->path[0] == '\0') { return false; }
  size_t len = kstrlen(file->path);
  if (len >= cap) { return false; }
  kmemcpy(out, file->path, len + 1);
  return true;
}

void cell_fd_set_dir_offset(int fd, uint64_t offset) {
  struct domain *domain = cell_current_domain_internal();
  if (cell_file_at(domain, fd) != NULL) { cell_file_at(domain, fd)->offset = offset; }
}
