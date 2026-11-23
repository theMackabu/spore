#include "internal.h"

#include "lib/kstr.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/pipe.h"
#include "proc/poll.h"
#include "proc/thread.h"
#include "fs/vfs.h"

enum {
  EBADF = 9,
  EMFILE = 24,
};

struct unix_right {
  bool used;
  uint8_t pipe_id;
  uint64_t start;
  uint64_t end;
  struct open_file *file;
};

struct unix_pending_conn {
  bool used;
  struct open_file *listener;
  struct open_file *server_end;
};

static struct unix_pending_conn unix_pending[16];

static struct unix_right unix_rights[32];

int cell_fd_socket_unix(void) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -24; }
  struct open_file *file = cell_alloc_open_file();
  if (file == NULL) { return -12; }
  file->type = OPEN_UNIX_STREAM;
  file->unix_owner_pid = domain->id;
  file->unix_owner_uid = domain->euid;
  file->unix_owner_gid = domain->egid;
  domain->fds[fd] = file;
  domain->fd_flags[fd] = 0;
  return fd;
}

int cell_fd_socketpair_unix(uint64_t sv_addr, int flags) {
  enum {
    CELL_O_NONBLOCK = 04000,
    CELL_O_CLOEXEC = 02000000,
  };
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  int fd0 = cell_find_free_fd(domain, 0);
  int fd1 = fd0 < 0 ? -1 : cell_find_free_fd(domain, fd0 + 1);
  if (fd0 < 0 || fd1 < 0) { return -24; }

  int a2b = cell_alloc_pipe_obj(0);
  int b2a = cell_alloc_pipe_obj(0);
  if (a2b < 0 || b2a < 0) {
    if (a2b >= 0) { cell_pipe_free((uint8_t)a2b); }
    if (b2a >= 0) { cell_pipe_free((uint8_t)b2a); }
    return -23;
  }

  struct open_file *a = cell_alloc_open_file();
  struct open_file *b = cell_alloc_open_file();
  if (a == NULL || b == NULL) {
    cell_release_open_file(a);
    cell_release_open_file(b);
    cell_pipe_free((uint8_t)a2b);
    cell_pipe_free((uint8_t)b2a);
    return -12;
  }

  cell_pipe_add_reader((uint8_t)a2b);
  cell_pipe_add_writer((uint8_t)a2b);
  cell_pipe_add_reader((uint8_t)b2a);
  cell_pipe_add_writer((uint8_t)b2a);

  a->type = OPEN_UNIX_STREAM;
  a->flags = (uint32_t)(flags & CELL_O_NONBLOCK);
  a->unix_connected = true;
  a->unix_rx_pipe = (uint8_t)b2a;
  a->unix_tx_pipe = (uint8_t)a2b;
  a->unix_owner_pid = domain->id;
  a->unix_owner_uid = domain->euid;
  a->unix_owner_gid = domain->egid;
  a->unix_peer_pid = domain->id;
  a->unix_peer_uid = domain->euid;
  a->unix_peer_gid = domain->egid;

  b->type = OPEN_UNIX_STREAM;
  b->flags = (uint32_t)(flags & CELL_O_NONBLOCK);
  b->unix_connected = true;
  b->unix_rx_pipe = (uint8_t)a2b;
  b->unix_tx_pipe = (uint8_t)b2a;
  b->unix_owner_pid = domain->id;
  b->unix_owner_uid = domain->euid;
  b->unix_owner_gid = domain->egid;
  b->unix_peer_pid = domain->id;
  b->unix_peer_uid = domain->euid;
  b->unix_peer_gid = domain->egid;

  int sv[2] = {fd0, fd1};
  if (!vmm_copy_to_user(cell_domain_as(domain), sv_addr, sv, sizeof(sv))) {
    cell_release_open_file(a);
    cell_release_open_file(b);
    return -14;
  }
  domain->fds[fd0] = a;
  domain->fds[fd1] = b;
  domain->fd_flags[fd0] = (flags & CELL_O_CLOEXEC) != 0 ? 1 : 0;
  domain->fd_flags[fd1] = (flags & CELL_O_CLOEXEC) != 0 ? 1 : 0;
  return 0;
}

static struct open_file *unix_file_for_fd(struct domain *domain, int fd) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return NULL; }
  struct open_file *file = domain->fds[fd];
  return file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER ? file : NULL;
}

static struct open_file *unix_listener_for_path(const char *path) {
  for (size_t d = 0; d < cell_domain_capacity(); ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file != NULL && file->type == OPEN_UNIX_LISTENER && str_eq(file->unix_path, path)) { return file; }
    }
  }
  return NULL;
}

static bool unix_listener_readable(const struct open_file *listener) {
  for (size_t i = 0; i < sizeof(unix_pending) / sizeof(unix_pending[0]); ++i) {
    if (unix_pending[i].used && unix_pending[i].listener == listener) { return true; }
  }
  return false;
}

int unix_poll_events(const struct open_file *file, int events) {
  if (file->type == OPEN_UNIX_LISTENER) {
    return (events & CELL_POLLIN) && unix_listener_readable(file) ? CELL_POLLIN : 0;
  }
  if (!file->unix_connected) { return 0; }
  int ready = cell_pipe_id_hup(file->unix_rx_pipe) ? CELL_POLLHUP : 0;
  if ((events & CELL_POLLIN) && cell_pipe_id_readable(file->unix_rx_pipe)) { ready |= CELL_POLLIN; }
  if ((events & CELL_POLLOUT) && cell_pipe_id_writable(file->unix_tx_pipe)) { ready |= CELL_POLLOUT; }
  return ready;
}

void cell_socket_release_listener(struct open_file *file) {
  for (size_t i = 0; i < sizeof(unix_pending) / sizeof(unix_pending[0]); ++i) {
    if (unix_pending[i].used && unix_pending[i].listener == file) {
      cell_release_open_file(unix_pending[i].server_end);
      unix_pending[i] = (struct unix_pending_conn){0};
    }
  }
}

int cell_fd_unix_bind(int fd, const char *path) {
  struct open_file *file = unix_file_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL || file->type != OPEN_UNIX_STREAM || path == NULL || path[0] == '\0') { return -EINVAL; }
  struct vfs_node node;
  if (vfs_lookup(path, &node)) { return -EADDRINUSE; }
  if (!vfs_mksock(path, 0666, &node)) { return -EINVAL; }
  copy_cstr(file->unix_path, sizeof(file->unix_path), path);
  file->node = node;
  return 0;
}

int cell_fd_unix_listen(int fd, int backlog) {
  (void)backlog;
  struct open_file *file = unix_file_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL || file->type != OPEN_UNIX_STREAM || file->unix_path[0] == '\0') { return -EINVAL; }
  file->type = OPEN_UNIX_LISTENER;
  return 0;
}

static int take_pending_unix(struct domain *domain, struct open_file *listener, int flags) {
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -24; }
  for (size_t i = 0; i < sizeof(unix_pending) / sizeof(unix_pending[0]); ++i) {
    if (!unix_pending[i].used || unix_pending[i].listener != listener) { continue; }
    domain->fds[fd] = unix_pending[i].server_end;
    domain->fd_flags[fd] = 0;
    socket_apply_accept_flags(domain, fd, flags);
    unix_pending[i] = (struct unix_pending_conn){0};
    return fd;
  }
  return -EAGAIN;
}

static void wake_unix_accept_waiters(struct open_file *listener) {
  for (size_t i = 0; i < cell_thread_capacity(); ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait.reason != WAIT_SOCKET ||
        thread->domain == NULL) {
      continue;
    }
    int fd = thread->wait.target;
    if (fd < 0 || fd >= MAX_FDS || thread->domain->fds[fd] != listener) { continue; }
    int accepted = take_pending_unix(thread->domain, listener, thread->wait.socket_accept_flags);
    if (accepted == -EAGAIN) { continue; }
    cell_finish_wait(thread, accepted);
  }
  cell_wake_poll_waiters_internal();
}

int cell_fd_unix_accept(int fd, int flags, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *listener = unix_file_for_fd(domain, fd);
  if (listener == NULL || listener->type != OPEN_UNIX_LISTENER) { return -9; }
  int accepted = take_pending_unix(domain, listener, flags);
  if (accepted != -EAGAIN) { return accepted; }
  if ((listener->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return -EAGAIN; }
  return cell_block_current_on_socket_accept(fd, 0, 0, flags, frame);
}

int cell_fd_unix_connect(int fd, const char *path) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *client = unix_file_for_fd(domain, fd);
  if (client == NULL || client->type != OPEN_UNIX_STREAM || path == NULL) { return -9; }
  struct open_file *listener = unix_listener_for_path(path);
  if (listener == NULL) { return -ECONNREFUSED; }
  int c2s = cell_alloc_pipe_obj(0);
  int s2c = cell_alloc_pipe_obj(0);
  if (c2s < 0 || s2c < 0) {
    if (c2s >= 0) { cell_pipe_free((uint8_t)c2s); }
    if (s2c >= 0) { cell_pipe_free((uint8_t)s2c); }
    return -23;
  }
  struct open_file *server = cell_alloc_open_file();
  if (server == NULL) {
    cell_pipe_free((uint8_t)c2s);
    cell_pipe_free((uint8_t)s2c);
    return -12;
  }
  cell_pipe_add_reader((uint8_t)c2s);
  cell_pipe_add_writer((uint8_t)c2s);
  cell_pipe_add_reader((uint8_t)s2c);
  cell_pipe_add_writer((uint8_t)s2c);
  client->unix_rx_pipe = (uint8_t)s2c;
  client->unix_tx_pipe = (uint8_t)c2s;
  client->unix_connected = true;
  client->unix_peer_pid = listener->unix_owner_pid;
  client->unix_peer_uid = listener->unix_owner_uid;
  client->unix_peer_gid = listener->unix_owner_gid;
  copy_cstr(client->unix_path, sizeof(client->unix_path), path);
  server->type = OPEN_UNIX_STREAM;
  server->unix_connected = true;
  server->unix_rx_pipe = (uint8_t)c2s;
  server->unix_tx_pipe = (uint8_t)s2c;
  server->unix_owner_pid = listener->unix_owner_pid;
  server->unix_owner_uid = listener->unix_owner_uid;
  server->unix_owner_gid = listener->unix_owner_gid;
  server->unix_peer_pid = domain->id;
  server->unix_peer_uid = domain->euid;
  server->unix_peer_gid = domain->egid;
  server->node = listener->node;
  copy_cstr(server->unix_path, sizeof(server->unix_path), path);
  for (size_t i = 0; i < sizeof(unix_pending) / sizeof(unix_pending[0]); ++i) {
    if (!unix_pending[i].used) {
      unix_pending[i] = (struct unix_pending_conn){.used = true, .listener = listener, .server_end = server};
      wake_unix_accept_waiters(listener);
      return 0;
    }
  }
  cell_release_open_file(server);
  cell_pipe_free((uint8_t)c2s);
  cell_pipe_free((uint8_t)s2c);
  return -EAGAIN;
}

int cell_fd_unix_queue_right_range(int fd, int passed_fd, uint64_t start, uint64_t end) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || passed_fd < 0 || passed_fd >= MAX_FDS || domain->fds[fd] == NULL ||
      domain->fds[passed_fd] == NULL) {
    return -EBADF;
  }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_UNIX_STREAM || !file->unix_connected) { return -ENOTCONN; }
  if (end < start) { return -EINVAL; }
  for (size_t i = 0; i < sizeof(unix_rights) / sizeof(unix_rights[0]); ++i) {
    if (unix_rights[i].used) { continue; }
    cell_retain_open_file(domain->fds[passed_fd]);
    unix_rights[i] = (struct unix_right){
      .used = true,
      .pipe_id = file->unix_tx_pipe,
      .start = start,
      .end = end,
      .file = domain->fds[passed_fd],
    };
    return 0;
  }
  return -EAGAIN;
}

int cell_fd_unix_recv_right_for_range(int fd, uint64_t start, uint64_t end, int fd_flags) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -EBADF; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_UNIX_STREAM || !file->unix_connected) { return -ENOTCONN; }
  for (size_t i = 0; i < sizeof(unix_rights) / sizeof(unix_rights[0]); ++i) {
    if (!unix_rights[i].used || unix_rights[i].pipe_id != file->unix_rx_pipe) { continue; }
    if (unix_rights[i].start < start || unix_rights[i].start >= end) { continue; }
    int newfd = cell_find_free_fd(domain, 0);
    if (newfd < 0) { return -EMFILE; }
    domain->fds[newfd] = unix_rights[i].file;
    domain->fd_flags[newfd] = fd_flags != 0 ? 1 : 0;
    unix_rights[i] = (struct unix_right){0};
    return newfd;
  }
  return -EAGAIN;
}

bool cell_fd_unix_tx_offset(int fd, uint64_t *out) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL || out == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  return file->type == OPEN_UNIX_STREAM && file->unix_connected && cell_pipe_id_write_offset(file->unix_tx_pipe, out);
}

bool cell_fd_unix_rx_offset(int fd, uint64_t *out) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL || out == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  return file->type == OPEN_UNIX_STREAM && file->unix_connected && cell_pipe_id_read_offset(file->unix_rx_pipe, out);
}

bool cell_fd_unix_next_right_range(int fd, uint64_t *start, uint64_t *end) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL || start == NULL || end == NULL) {
    return false;
  }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_UNIX_STREAM || !file->unix_connected) { return false; }
  bool found = false;
  uint64_t best_start = 0;
  uint64_t best_end = 0;
  for (size_t i = 0; i < sizeof(unix_rights) / sizeof(unix_rights[0]); ++i) {
    if (!unix_rights[i].used || unix_rights[i].pipe_id != file->unix_rx_pipe) { continue; }
    if (!found || unix_rights[i].start < best_start) {
      found = true;
      best_start = unix_rights[i].start;
      best_end = unix_rights[i].end;
    }
  }
  if (!found) { return false; }
  *start = best_start;
  *end = best_end;
  return true;
}

void cell_unix_release_rights_for_pipe(uint8_t pipe_id) {
  for (size_t i = 0; i < sizeof(unix_rights) / sizeof(unix_rights[0]); ++i) {
    if (!unix_rights[i].used || unix_rights[i].pipe_id != pipe_id) { continue; }
    cell_release_open_file(unix_rights[i].file);
    unix_rights[i] = (struct unix_right){0};
  }
}

bool cell_fd_unix_peer_cred(int fd, struct cell_peer_cred *out) {
  struct open_file *file = unix_file_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL || out == NULL || file->type != OPEN_UNIX_STREAM || file->unix_peer_pid <= 0) { return false; }
  out->pid = file->unix_peer_pid;
  out->uid = file->unix_peer_uid;
  out->gid = file->unix_peer_gid;
  return true;
}
