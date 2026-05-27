#include "proc/socket.h"

#include "kstr.h"
#include "mem.h"
#include "net.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/pipe.h"
#include "proc/poll.h"
#include "proc/thread.h"
#include "vfs.h"

enum {
  CELL_O_NONBLOCK = 04000,
  SOCK_STREAM = 1,
  SOCK_DGRAM = 2,
  IPPROTO_TCP = 6,
  IPPROTO_UDP = 17,
  IPPROTO_ICMP = 1,
  EPERM = 1,
  EMSGSIZE = 90,
  EAGAIN = 11,
  EFAULT = 14,
  EINVAL = 22,
  EIO = 5,
  EINPROGRESS = 115,
  ENOTCONN = 107,
  EPIPE = 32,
  ECONNREFUSED = 111,
  EADDRINUSE = 98,
};

enum tcp_socket_state {
  TCP_CLOSED,
  TCP_SYN_SENT,
  TCP_ESTABLISHED,
  TCP_FIN_WAIT,
};

struct unix_pending_conn {
  bool used;
  struct open_file *listener;
  struct open_file *server_end;
};

static struct unix_pending_conn unix_pending[16];
static uint16_t next_tcp_port = 49152;
static uint16_t next_udp_port = 49152;

static uint16_t tcp_window(const struct open_file *file) {
  uint32_t room = sizeof(file->tcp_rx) - file->tcp_rx_len;
  return room > UINT16_MAX ? UINT16_MAX : (uint16_t)room;
}

int64_t cell_socket_tcp_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (domain == NULL || file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  if (file->tcp_state != TCP_ESTABLISHED) { return -ENOTCONN; }
  if (len == 0) { return 0; }
  uint64_t chunk = len > 1400 ? 1400 : len;
  uint8_t tmp[1400];
  if (!vmm_copy_from_user(&domain->as, tmp, buf, (size_t)chunk)) { return -EFAULT; }
  if (!net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                            file->tcp_ack, tcp_window(file), 0x18, tmp, (size_t)chunk)) {
    return -EIO;
  }
  file->tcp_seq += (uint32_t)chunk;
  return (int64_t)chunk;
}

int64_t cell_socket_tcp_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (file->tcp_error != 0) { return -(int64_t)file->tcp_error; }
  if (file->tcp_rx_len == 0) { return file->tcp_fin ? 0 : -EAGAIN; }
  uint64_t n = file->tcp_rx_len < len ? file->tcp_rx_len : len;
  if (!vmm_copy_to_user(&domain->as, buf, file->tcp_rx, (size_t)n)) { return -EFAULT; }
  if (n < file->tcp_rx_len) {
    uint32_t remaining = file->tcp_rx_len - (uint32_t)n;
    for (uint32_t i = 0; i < remaining; ++i) { file->tcp_rx[i] = file->tcp_rx[n + i]; }
  }
  file->tcp_rx_len -= (uint32_t)n;
  if (file->tcp_state == TCP_ESTABLISHED || file->tcp_state == TCP_FIN_WAIT) {
    (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                               file->tcp_ack, tcp_window(file), 0x10, NULL, 0);
  }
  return (int64_t)n;
}

int cell_fd_socket_inet(uint8_t proto) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
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
  file->type = OPEN_SOCKET;
  file->socket_proto = proto;
  file->udp_local_port = (uint16_t)(40000 + fd);
  domain->fds[fd] = file;
  return fd;
}

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
  return fd;
}

bool cell_fd_socket_info(int fd, int32_t *type, int32_t *proto) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER) {
    if (type != NULL) { *type = SOCK_STREAM; }
    if (proto != NULL) { *proto = 0; }
    return true;
  }
  if (file->type != OPEN_SOCKET) { return false; }
  if (type != NULL) { *type = file->socket_proto == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM; }
  if (proto != NULL) { *proto = file->socket_proto; }
  return true;
}

static struct open_file *unix_file_for_fd(struct domain *domain, int fd) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return NULL; }
  struct open_file *file = domain->fds[fd];
  return file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER ? file : NULL;
}

static struct open_file *unix_listener_for_path(const char *path) {
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file != NULL && file->type == OPEN_UNIX_LISTENER && str_eq(file->unix_path, path)) { return file; }
    }
  }
  return NULL;
}

bool cell_unix_listener_readable(const struct open_file *listener) {
  for (size_t i = 0; i < sizeof(unix_pending) / sizeof(unix_pending[0]); ++i) {
    if (unix_pending[i].used && unix_pending[i].listener == listener) { return true; }
  }
  return false;
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

int cell_socket_take_pending_unix(struct domain *domain, struct open_file *listener) {
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -24; }
  for (size_t i = 0; i < sizeof(unix_pending) / sizeof(unix_pending[0]); ++i) {
    if (!unix_pending[i].used || unix_pending[i].listener != listener) { continue; }
    domain->fds[fd] = unix_pending[i].server_end;
    unix_pending[i] = (struct unix_pending_conn){0};
    return fd;
  }
  return -EAGAIN;
}

int cell_fd_unix_accept(int fd, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *listener = unix_file_for_fd(domain, fd);
  if (listener == NULL || listener->type != OPEN_UNIX_LISTENER) { return -9; }
  int accepted = cell_socket_take_pending_unix(domain, listener);
  if (accepted != -EAGAIN) { return accepted; }
  if ((listener->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return -EAGAIN; }
  return cell_block_current_on_socket(fd, 0, 0, 0, 0, frame);
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
  client->unix_peer_pid = listener->unix_owner_pid;
  client->unix_peer_uid = listener->unix_owner_uid;
  client->unix_peer_gid = listener->unix_owner_gid;
  copy_cstr(client->unix_path, sizeof(client->unix_path), path);
  server->type = OPEN_UNIX_STREAM;
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
      cell_socket_wake_unix_accept_waiters(listener);
      return 0;
    }
  }
  cell_release_open_file(server);
  cell_pipe_free((uint8_t)c2s);
  cell_pipe_free((uint8_t)s2c);
  return -EAGAIN;
}

bool cell_fd_unix_peer_cred(int fd, struct cell_peer_cred *out) {
  struct open_file *file = unix_file_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL || out == NULL || file->type != OPEN_UNIX_STREAM || file->unix_peer_pid <= 0) { return false; }
  out->pid = file->unix_peer_pid;
  out->uid = file->unix_peer_uid;
  out->gid = file->unix_peer_gid;
  return true;
}

static struct open_file *udp_socket_for_fd(struct domain *domain, int fd) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL || domain->fds[fd]->type != OPEN_SOCKET) {
    return NULL;
  }
  return domain->fds[fd];
}

bool cell_fd_udp_bind(int fd, uint16_t port) {
  struct open_file *file = udp_socket_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL) { return false; }
  if (port == 0) {
    port = next_udp_port++;
    if (next_udp_port < 49152) { next_udp_port = 49152; }
  }
  file->udp_local_port = port;
  return true;
}

bool cell_fd_udp_connect(int fd, uint32_t ip, uint16_t port) {
  struct open_file *file = udp_socket_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL) { return false; }
  file->udp_remote_ip = ip;
  file->udp_remote_port = port;
  file->udp_connected = true;
  return true;
}

static struct open_file *tcp_socket_for_fd(struct domain *domain, int fd) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL || domain->fds[fd]->type != OPEN_SOCKET ||
      domain->fds[fd]->socket_proto != IPPROTO_TCP) {
    return NULL;
  }
  return domain->fds[fd];
}

static uint32_t tcp_initial_seq(struct domain *domain, int fd) {
  uint32_t pid = domain == NULL ? 0 : (uint32_t)domain->id;
  return 0x50000000u + pid * 4096u + (uint32_t)fd * 97u;
}

int cell_fd_tcp_connect(int fd, uint32_t ip, uint16_t port, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = tcp_socket_for_fd(domain, fd);
  if (file == NULL) { return -9; }
  if (file->tcp_state == TCP_ESTABLISHED) { return 0; }
  if (!cell_egress_allowed(IPPROTO_TCP, ip, port)) { return -EPERM; }
  if (file->tcp_state == TCP_CLOSED) {
    file->tcp_remote_ip = ip;
    file->tcp_remote_port = port;
    file->tcp_local_port = next_tcp_port++;
    if (next_tcp_port < 49152) { next_tcp_port = 49152; }
    file->tcp_seq = tcp_initial_seq(domain, fd);
    file->tcp_ack = 0;
    file->tcp_error = 0;
    file->tcp_fin = false;
    file->tcp_state = TCP_SYN_SENT;
    if (!net_tcp_send_segment(file->tcp_local_port, ip, port, file->tcp_seq, 0, tcp_window(file), 0x02, NULL, 0)) {
      file->tcp_state = TCP_CLOSED;
      return -EIO;
    }
    file->tcp_seq += 1;
  }
  net_poll();
  if (file->tcp_error != 0) { return -(int)file->tcp_error; }
  if (file->tcp_state == TCP_ESTABLISHED) { return 0; }
  if ((file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return -EINPROGRESS; }
  return cell_block_current_on_socket(fd, 0, 0, 0, 0, frame);
}

int64_t cell_fd_tcp_send(int fd, uint64_t buf, uint64_t len) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = tcp_socket_for_fd(domain, fd);
  return cell_socket_tcp_write_from_domain(domain, file, buf, len);
}

int64_t cell_fd_udp_send(int fd, uint32_t ip, uint16_t port, uint64_t buf, uint64_t len) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = udp_socket_for_fd(domain, fd);
  if (file == NULL) { return -9; }
  uint32_t effective_ip = ip;
  uint16_t effective_port = port;
  if (effective_ip == 0 && effective_port == 0 && file->udp_connected) {
    effective_ip = file->udp_remote_ip;
    effective_port = file->udp_remote_port;
  }
  if (effective_ip == 0 || (file->socket_proto == IPPROTO_UDP && effective_port == 0)) { return -EINVAL; }
  if (file->socket_proto == IPPROTO_UDP && !cell_egress_allowed(IPPROTO_UDP, effective_ip, effective_port)) {
    return -EPERM;
  }
  uint8_t tmp[1472];
  if (len > sizeof(tmp)) { return -EMSGSIZE; }
  if (!vmm_copy_from_user(&domain->as, tmp, buf, (size_t)len)) { return -EFAULT; }
  bool sent = false;
  if (file->socket_proto == IPPROTO_UDP) {
    sent = net_udp_send(file->udp_local_port, effective_ip, effective_port, tmp, (size_t)len);
  } else if (file->socket_proto == IPPROTO_ICMP) {
    sent = net_icmp_send_echo(effective_ip, tmp, (size_t)len);
  }
  if (!sent) {
    if (file->socket_proto == IPPROTO_UDP && !cell_egress_allowed(IPPROTO_UDP, effective_ip, effective_port)) {
      return -EPERM;
    }
    return -EIO;
  }
  return (int64_t)len;
}

struct sockaddr_in_cell {
  uint16_t sin_family;
  uint16_t sin_port;
  uint32_t sin_addr;
  uint8_t sin_zero[8];
};

static uint16_t net_bswap16(uint16_t v) {
  return (uint16_t)((v << 8) | (v >> 8));
}

bool cell_socket_copy_udp_source_to_domain(struct domain *domain, const struct open_file *file, uint64_t addr,
                                           uint64_t addrlen) {
  if (addr == 0 && addrlen == 0) { return true; }
  if (addrlen != 0) {
    uint32_t len = sizeof(struct sockaddr_in_cell);
    if (!vmm_copy_to_user(&domain->as, addrlen, &len, sizeof(len))) { return false; }
  }
  if (addr == 0) { return true; }
  struct sockaddr_in_cell sa;
  kmemset(&sa, 0, sizeof(sa));
  sa.sin_family = 2;
  sa.sin_port = net_bswap16(file->udp_rx_port);
  sa.sin_addr = file->udp_rx_ip;
  return vmm_copy_to_user(&domain->as, addr, &sa, sizeof(sa));
}

int64_t cell_fd_socket_recv(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame, uint64_t addr,
                            uint64_t addrlen) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_UNIX_STREAM) {
    int64_t got = cell_pipe_read_id_to_domain(domain, file->unix_rx_pipe, buf, len);
    if (got != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return got; }
    return cell_block_current_on_pipe(fd, buf, len, false, frame);
  }
  if (file->type != OPEN_SOCKET) { return -9; }
  net_poll();
  if (file->socket_proto == IPPROTO_TCP) {
    int64_t got = cell_socket_tcp_read_to_domain(domain, file, buf, len);
    if (got != -EAGAIN || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return got; }
    return cell_block_current_on_socket(fd, buf, len, 0, 0, frame);
  }
  if (file->udp_rx_len == 0) {
    if ((file->flags & CELL_O_NONBLOCK) != 0) { return -EAGAIN; }
    if (frame == NULL) { return -EAGAIN; }
    return cell_block_current_on_socket(fd, buf, len, addr, addrlen, frame);
  }
  uint64_t n = file->udp_rx_len < len ? file->udp_rx_len : len;
  if (!vmm_copy_to_user(&domain->as, buf, file->udp_rx, (size_t)n)) { return -EFAULT; }
  if (!cell_socket_copy_udp_source_to_domain(domain, file, addr, addrlen)) { return -EFAULT; }
  file->udp_rx_len = 0;
  return (int64_t)n;
}

static bool socket_matches_udp(struct open_file *file, uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_UDP &&
         file->udp_local_port == dst_port &&
         (!file->udp_connected || (file->udp_remote_ip == src_ip && file->udp_remote_port == src_port));
}

void cell_net_deliver_udp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const void *payload, size_t len) {
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (!socket_matches_udp(file, src_ip, src_port, dst_port)) { continue; }
      if (len > sizeof(file->udp_rx)) { len = sizeof(file->udp_rx); }
      kmemcpy(file->udp_rx, payload, len);
      file->udp_rx_len = len;
      file->udp_rx_ip = src_ip;
      file->udp_rx_port = src_port;
      cell_socket_wake_file(file);
      return;
    }
  }
}

static bool socket_matches_tcp(struct open_file *file, uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP &&
         file->tcp_local_port == dst_port && file->tcp_remote_ip == src_ip && file->tcp_remote_port == src_port;
}

void cell_net_deliver_tcp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack,
                          uint8_t flags, const void *payload, size_t len) {
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (!socket_matches_tcp(file, src_ip, src_port, dst_port)) { continue; }
      if (file->tcp_state == TCP_SYN_SENT && (flags & 0x12) == 0x12 && ack == file->tcp_seq) {
        file->tcp_ack = seq + 1;
        file->tcp_state = TCP_ESTABLISHED;
        (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                                   file->tcp_ack, tcp_window(file), 0x10, NULL, 0);
        cell_socket_wake_file(file);
        return;
      }
      if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT) { return; }
      if ((flags & 0x04) != 0) {
        file->tcp_error = ECONNREFUSED;
        cell_socket_wake_file(file);
        return;
      }
      if (len != 0) {
        uint32_t room = sizeof(file->tcp_rx) - file->tcp_rx_len;
        uint64_t n = len < room ? len : room;
        if (n != 0) {
          kmemcpy(file->tcp_rx + file->tcp_rx_len, payload, (size_t)n);
          file->tcp_rx_len += (uint32_t)n;
          file->tcp_ack = seq + (uint32_t)n;
          (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                                     file->tcp_ack, tcp_window(file), 0x10, NULL, 0);
        } else {
          (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                                     file->tcp_ack, 0, 0x10, NULL, 0);
        }
      }
      if ((flags & 0x01) != 0) {
        file->tcp_fin = true;
        file->tcp_ack = seq + (uint32_t)len + 1;
        (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                                   file->tcp_ack, tcp_window(file), 0x10, NULL, 0);
      }
      cell_socket_wake_file(file);
      return;
    }
  }
}

void cell_net_deliver_icmp(uint32_t src_ip, const void *payload, size_t len) {
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_ICMP) { continue; }
      if (len > sizeof(file->udp_rx)) { len = sizeof(file->udp_rx); }
      kmemcpy(file->udp_rx, payload, len);
      file->udp_rx_len = len;
      file->udp_rx_ip = src_ip;
      cell_socket_wake_file(file);
      return;
    }
  }
}

static void wake_socket_waiters(struct open_file *file) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_SOCKET ||
        thread->domain == NULL) {
      continue;
    }
    int fd = thread->wait_target;
    if (fd >= 0 && fd < MAX_FDS && thread->domain->fds[fd] == file) {
      if (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP) {
        if (thread->pipe_buf == 0 && thread->pipe_len == 0) {
          if (file->tcp_error != 0) {
            thread->tf.x[0] = (uint64_t)(-(int64_t)file->tcp_error);
          } else if (file->tcp_state != TCP_ESTABLISHED) {
            continue;
          } else {
            thread->tf.x[0] = 0;
          }
        } else {
          int64_t rc = cell_socket_tcp_read_to_domain(thread->domain, file, thread->pipe_buf, thread->pipe_len);
          if (rc == -EAGAIN) { continue; }
          thread->tf.x[0] = (uint64_t)rc;
        }
      } else if (file->type == OPEN_SOCKET && file->udp_rx_len != 0 && thread->pipe_buf != 0) {
        uint64_t n = file->udp_rx_len < thread->pipe_len ? file->udp_rx_len : thread->pipe_len;
        if (!vmm_copy_to_user(&thread->domain->as, thread->pipe_buf, file->udp_rx, (size_t)n)) {
          thread->tf.x[0] = (uint64_t)(-(int64_t)EFAULT);
        } else if (!cell_socket_copy_udp_source_to_domain(thread->domain, file, thread->socket_addr,
                                                          thread->socket_addrlen)) {
          thread->tf.x[0] = (uint64_t)(-(int64_t)EFAULT);
        } else {
          file->udp_rx_len = 0;
          thread->tf.x[0] = (uint64_t)n;
        }
      }
      thread->state = THREAD_RUNNABLE;
      thread->wait_reason = WAIT_NONE;
      thread->wait_target = -1;
      thread->pipe_buf = 0;
      thread->pipe_len = 0;
      thread->socket_addr = 0;
      thread->socket_addrlen = 0;
    }
  }
  cell_wake_poll_waiters_internal();
}

void cell_socket_wake_file(struct open_file *file) {
  wake_socket_waiters(file);
}
