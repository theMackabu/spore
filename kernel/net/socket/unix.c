#include "internal.h"

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
  struct pipe_obj *pipe;
  uint64_t start;
  uint64_t end;
  struct open_file *file;
};

static struct open_file *listeners[32];

static struct open_file **listener_bucket(const struct vfs_node *node) {
  return &listeners[node->ino % (sizeof(listeners) / sizeof(*listeners))];
}

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

  struct pipe_obj *a2b = cell_pipe_create();
  struct pipe_obj *b2a = cell_pipe_create();
  if (!a2b || !b2a) {
    cell_pipe_discard(a2b);
    cell_pipe_discard(b2a);
    return -23;
  }

  struct open_file *a = cell_alloc_open_file();
  struct open_file *b = cell_alloc_open_file();
  if (a == NULL || b == NULL) {
    cell_release_open_file(a);
    cell_release_open_file(b);
    cell_pipe_discard(a2b);
    cell_pipe_discard(b2a);
    return -12;
  }

  cell_pipe_add_reader(a2b);
  cell_pipe_add_writer(a2b);
  cell_pipe_add_reader(b2a);
  cell_pipe_add_writer(b2a);

  a->type = OPEN_UNIX_STREAM;
  a->flags = (uint32_t)(flags & CELL_O_NONBLOCK);
  a->unix_connected = true;
  a->unix_rx_pipe = b2a;
  a->unix_tx_pipe = a2b;
  a->unix_owner_pid = domain->id;
  a->unix_owner_uid = domain->euid;
  a->unix_owner_gid = domain->egid;
  a->unix_peer_pid = domain->id;
  a->unix_peer_uid = domain->euid;
  a->unix_peer_gid = domain->egid;

  b->type = OPEN_UNIX_STREAM;
  b->flags = (uint32_t)(flags & CELL_O_NONBLOCK);
  b->unix_connected = true;
  b->unix_rx_pipe = a2b;
  b->unix_tx_pipe = b2a;
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

static struct open_file *unix_listener_for_node(const struct vfs_node *node) {
  for (struct open_file *file = *listener_bucket(node); file; file = file->socket_index_next) {
    if (file->node.dev_id == node->dev_id && file->node.ino == node->ino) { return file; }
  }
  return NULL;
}

int unix_poll_events(const struct open_file *file, int events) {
  if (file->type == OPEN_UNIX_LISTENER) {
    return (events & CELL_POLLIN) && file->unix_pending_head ? CELL_POLLIN : 0;
  }
  if (!file->unix_connected) { return 0; }
  int ready = cell_pipe_hup(file->unix_rx_pipe) ? CELL_POLLHUP : 0;
  if ((events & CELL_POLLIN) && cell_pipe_readable(file->unix_rx_pipe)) { ready |= CELL_POLLIN; }
  if ((events & CELL_POLLOUT) && cell_pipe_writable(file->unix_tx_pipe)) { ready |= CELL_POLLOUT; }
  return ready;
}

static struct open_file *take_head(struct open_file *listener) {
  struct open_file *server = listener->unix_pending_head;
  listener->unix_pending_head = server->unix_pending_next;
  if (!listener->unix_pending_head) { listener->unix_pending_tail = NULL; }
  server->unix_pending_next = NULL;
  --listener->unix_pending_count;
  return server;
}

void cell_unix_release_file(struct open_file *file) {
  if (file->type == OPEN_UNIX_LISTENER) {
    socket_index_remove(listener_bucket(&file->node), file);
    while (file->unix_pending_head) { cell_release_open_file(take_head(file)); }
  }
  if (file->unix_connected) {
    cell_pipe_drop_reader(file->unix_rx_pipe);
    cell_pipe_drop_writer(file->unix_tx_pipe);
    cell_pipe_notify();
  }
  if (file->unix_bound) { vfs_release_node(&file->node); }
}

int cell_fd_unix_bind(int fd, const char *path) {
  struct open_file *file = unix_file_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL || file->type != OPEN_UNIX_STREAM || path == NULL || path[0] == '\0') { return -EINVAL; }
  struct vfs_node node;
  if (file->unix_bound) { return -EINVAL; }
  if (vfs_lookup(path, &node)) { return -EADDRINUSE; }
  if (!vfs_mksock(path, 0666, &node)) { return -EINVAL; }
  if (!vfs_retain_node(&node)) { (void)vfs_remove(path); return -12; }
  file->unix_bound = true;
  file->node = node;
  return 0;
}

int cell_fd_unix_listen(int fd, int backlog) {
  struct open_file *file = unix_file_for_fd(cell_current_domain_internal(), fd);
  if (!file || !file->unix_bound || file->unix_connected) { return -EINVAL; }
  if (file->type != OPEN_UNIX_LISTENER) {
    file->type = OPEN_UNIX_LISTENER;
    socket_index_insert(listener_bucket(&file->node), file);
  }
  file->unix_backlog = backlog <= 0 ? 1 : (uint16_t)(backlog > MAX_OPEN_FILES ? MAX_OPEN_FILES : backlog);
  return 0;
}

int take_pending_unix(struct domain *domain, struct open_file *listener, int flags) {
  if (!listener->unix_pending_head) { return -EAGAIN; }
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -EMFILE; }
  domain->fds[fd] = take_head(listener);
  domain->fd_flags[fd] = 0;
  socket_apply_accept_flags(domain, fd, flags);
  return fd;
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
  if (client->unix_connected) { return -106; }
  struct vfs_node node;
  if (!vfs_lookup(path, &node)) { return -2; }
  struct open_file *listener = unix_listener_for_node(&node);
  if (listener == NULL) { return -ECONNREFUSED; }
  if (listener->unix_pending_count >= listener->unix_backlog) { return -EAGAIN; }
  struct pipe_obj *c2s = cell_pipe_create();
  struct pipe_obj *s2c = cell_pipe_create();
  if (!c2s || !s2c) {
    cell_pipe_discard(c2s);
    cell_pipe_discard(s2c);
    return -23;
  }
  struct open_file *server = cell_alloc_open_file();
  if (server == NULL) {
    cell_pipe_discard(c2s);
    cell_pipe_discard(s2c);
    return -12;
  }
  cell_pipe_add_reader(c2s);
  cell_pipe_add_writer(c2s);
  cell_pipe_add_reader(s2c);
  cell_pipe_add_writer(s2c);
  client->unix_rx_pipe = s2c;
  client->unix_tx_pipe = c2s;
  client->unix_connected = true;
  client->unix_peer_pid = listener->unix_owner_pid;
  client->unix_peer_uid = listener->unix_owner_uid;
  client->unix_peer_gid = listener->unix_owner_gid;
  server->type = OPEN_UNIX_STREAM;
  server->unix_connected = true;
  server->unix_rx_pipe = c2s;
  server->unix_tx_pipe = s2c;
  server->unix_owner_pid = listener->unix_owner_pid;
  server->unix_owner_uid = listener->unix_owner_uid;
  server->unix_owner_gid = listener->unix_owner_gid;
  server->unix_peer_pid = domain->id;
  server->unix_peer_uid = domain->euid;
  server->unix_peer_gid = domain->egid;
  server->node = listener->node;
  if (listener->unix_pending_tail) { listener->unix_pending_tail->unix_pending_next = server; }
  else { listener->unix_pending_head = server; }
  listener->unix_pending_tail = server;
  ++listener->unix_pending_count;
  cell_socket_wake_file(listener);
  return 0;
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
      .pipe = file->unix_tx_pipe,
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
    if (!unix_rights[i].used || unix_rights[i].pipe != file->unix_rx_pipe) { continue; }
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
  return file->type == OPEN_UNIX_STREAM && file->unix_connected && cell_pipe_write_offset(file->unix_tx_pipe, out);
}

bool cell_fd_unix_rx_offset(int fd, uint64_t *out) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL || out == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  return file->type == OPEN_UNIX_STREAM && file->unix_connected && cell_pipe_read_offset(file->unix_rx_pipe, out);
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
    if (!unix_rights[i].used || unix_rights[i].pipe != file->unix_rx_pipe) { continue; }
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

void cell_unix_release_rights_for_pipe(struct pipe_obj *pipe) {
  for (size_t i = 0; i < sizeof(unix_rights) / sizeof(unix_rights[0]); ++i) {
    if (!unix_rights[i].used || unix_rights[i].pipe != pipe) { continue; }
    struct open_file *file = unix_rights[i].file;
    unix_rights[i] = (struct unix_right){0};
    cell_release_open_file(file);
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
