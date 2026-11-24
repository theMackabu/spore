#include "internal.h"

#include "lib/mem.h"
#include "net/net.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/poll.h"
#include "lib/random.h"

enum {
  EMSGSIZE = 90,
  EACCES = 13,
  UDP_PORT_BUCKETS = 64,
};

static uint16_t next_udp_port = 49152;

static struct open_file *udp_port_buckets[UDP_PORT_BUCKETS];

static struct open_file **udp_port_bucket(uint16_t port) {
  return &udp_port_buckets[port % UDP_PORT_BUCKETS];
}

static void udp_set_local_port(struct open_file *file, uint16_t port) {
  if (file->udp_local_port == port) { return; }
  file->udp_local_port = port;
  socket_index_insert(udp_port_bucket(port), file);
}

int dgram_poll_events(const struct open_file *file, int events) {
  int ready = file->socket_proto == IPPROTO_UDP && file->udp_error ? CELL_POLLERR : 0;
  if (events & CELL_POLLIN) {
    net_poll();
    if (file->dgram_rx_count) { ready |= CELL_POLLIN; }
  }
  return ready | (events & CELL_POLLOUT);
}

void udp_release_file(struct open_file *file) {
  dgram_clear_rx(file);
  socket_index_remove(udp_port_bucket(file->udp_local_port), file);
}

static struct open_file *udp_socket_for_fd(struct domain *domain, int fd) {
  if (cell_file_at(domain, fd) == NULL || cell_file_at(domain, fd)->type != OPEN_SOCKET) {
    return NULL;
  }
  struct open_file *file = cell_file_at(domain, fd);
  return file->socket_proto == IPPROTO_UDP || file->socket_proto == IPPROTO_ICMP ? file : NULL;
}

static bool udp_local_port_in_use(uint16_t port, const struct open_file *except) {
  if (port == 0) { return false; }
  for (struct open_file *file = *udp_port_bucket(port); file != NULL;
       file = file->socket_index_next) {
    if (file != except && file->type == OPEN_SOCKET && file->udp_local_port == port) {
      return true;
    }
  }
  return false;
}

static uint16_t udp_ephemeral_port(const struct open_file *file) {
  uint16_t first = next_udp_port;
  do {
    uint16_t port = next_udp_port++;
    if (next_udp_port < 49152) { next_udp_port = 49152; }
    if (!udp_local_port_in_use(port, file)) { return port; }
  } while (next_udp_port != first);
  return 0;
}

int cell_fd_udp_bind(int fd, uint16_t port) {
  struct open_file *file = udp_socket_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL) { return -9; }
  if (file->socket_proto != IPPROTO_UDP) { return -EINVAL; }
  if (file->udp_local_port != 0 && file->udp_local_port != port) { return -EINVAL; }
  if (port == 0) {
    port = udp_ephemeral_port(file);
    if (port == 0) { return -EADDRINUSE; }
  } else if (udp_local_port_in_use(port, file)) {
    return -EADDRINUSE;
  }
  udp_set_local_port(file, port);
  return 0;
}

bool cell_fd_udp_connect(int fd, uint32_t ip, uint16_t port) {
  struct open_file *file = udp_socket_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL || file->socket_proto != IPPROTO_UDP) { return false; }
  if (file->udp_local_port == 0) {
    uint16_t local_port = udp_ephemeral_port(file);
    if (local_port == 0) { return false; }
    udp_set_local_port(file, local_port);
  }
  file->udp_remote_ip = ip;
  file->udp_remote_port = port;
  file->udp_connected = true;
  file->udp_error = 0;
  return true;
}

int cell_fd_udp_disconnect(int fd) {
  struct open_file *file = udp_socket_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL) { return -9; }
  if (file->socket_proto != IPPROTO_UDP) { return -EINVAL; }
  file->udp_remote_ip = 0;
  file->udp_remote_port = 0;
  file->udp_connected = false;
  file->udp_error = 0;
  return 0;
}

static int64_t udp_send_payload(struct open_file *file, uint32_t ip, uint16_t port, const void *buf, uint64_t len) {
  if (file == NULL) { return -9; }
  if (buf == NULL && len != 0) { return -EFAULT; }
  uint32_t effective_ip = ip;
  uint16_t effective_port = port;
  if (effective_ip == 0 && effective_port == 0 && file->udp_connected) {
    effective_ip = file->udp_remote_ip;
    effective_port = file->udp_remote_port;
  }
  if (effective_ip == 0 || (file->socket_proto == IPPROTO_UDP && effective_port == 0)) { return -EINVAL; }
  if (file->socket_proto == IPPROTO_UDP && net_is_broadcast_addr(effective_ip) && !file->so_broadcast) {
    return -EACCES;
  }
  if (!cell_egress_allowed(file->socket_proto, effective_ip, effective_port)) {
    return -EPERM;
  }
  if (file->udp_local_port == 0) {
    uint16_t local_port = udp_ephemeral_port(file);
    if (local_port == 0) { return -EADDRINUSE; }
    udp_set_local_port(file, local_port);
  }
  if (len > CELL_DGRAM_RX_CAP) { return -EMSGSIZE; }
  bool sent = false;
  if (file->socket_proto == IPPROTO_UDP) {
    sent = net_udp_send_ttl_tos(file->udp_local_port, effective_ip, effective_port, socket_effective_ttl(file),
                                (uint8_t)file->ip_tos, buf, (size_t)len);
  } else if (file->socket_proto == IPPROTO_ICMP) {
    sent = net_icmp_send_echo(effective_ip, file->udp_local_port, socket_effective_ttl(file), (uint8_t)file->ip_tos, buf, (size_t)len);
  }
  if (!sent) {
    if (file->socket_proto == IPPROTO_UDP && net_is_loopback_addr(effective_ip)) {
      if (file->udp_connected && file->udp_remote_ip == effective_ip && file->udp_remote_port == effective_port) {
        file->udp_error = ECONNREFUSED;
        cell_socket_wake_file(file);
      }
      return (int64_t)len;
    }
    if (!cell_egress_allowed(file->socket_proto, effective_ip, effective_port)) {
      return -EPERM;
    }
    return -EIO;
  }
  return (int64_t)len;
}

int64_t cell_fd_udp_send(int fd, uint32_t ip, uint16_t port, uint64_t buf, uint64_t len) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = udp_socket_for_fd(domain, fd);
  if (file == NULL) { return -9; }
  uint8_t tmp[1472];
  if (len > sizeof(tmp)) { return -EMSGSIZE; }
  if (!vmm_copy_from_user(cell_domain_as(domain), tmp, buf, (size_t)len)) { return -EFAULT; }
  return udp_send_payload(file, ip, port, tmp, len);
}

int64_t cell_fd_udp_send_kernel(int fd, uint32_t ip, uint16_t port, const void *buf, uint64_t len) {
  return udp_send_payload(udp_socket_for_fd(cell_current_domain_internal(), fd), ip, port, buf, len);
}

static bool socket_matches_udp(struct open_file *file, uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_UDP &&
         file->udp_local_port == dst_port &&
         (!file->udp_connected || (file->udp_remote_ip == src_ip && file->udp_remote_port == src_port));
}

bool cell_net_deliver_udp(uint32_t src_ip, uint8_t ttl, uint16_t src_port, uint16_t dst_port, const void *payload, size_t len) {
  for (struct open_file *file = *udp_port_bucket(dst_port); file != NULL;
       file = file->socket_index_next) {
    if (!socket_matches_udp(file, src_ip, src_port, dst_port)) { continue; }
    if (dgram_push_rx(file, src_ip, ttl, src_port, payload, len)) { cell_socket_wake_file(file); }
    return true;
  }
  return false;
}

bool cell_net_deliver_udp_error(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port, int error) {
  for (struct open_file *file = *udp_port_bucket(local_port); file != NULL;
       file = file->socket_index_next) {
    if (file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_UDP || !file->udp_connected ||
        file->udp_local_port != local_port || file->udp_remote_ip != remote_ip || file->udp_remote_port != remote_port) {
      continue;
    }
    file->udp_error = (uint32_t)error;
    cell_socket_wake_file(file);
    return true;
  }
  return false;
}

void cell_net_deliver_icmp(uint32_t src_ip, uint8_t ttl, const void *payload, size_t len) {
  if (len < 8) { return; }
  const uint8_t *icmp = payload;
  uint16_t id = (uint16_t)((icmp[4] << 8) | icmp[5]);
  for (struct open_file *file = *udp_port_bucket(id); file != NULL;
       file = file->socket_index_next) {
    if (file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_ICMP || file->udp_local_port != id) { continue; }
    if (dgram_push_rx(file, src_ip, ttl, 0, payload, len)) { cell_socket_wake_file(file); }
    return;
  }
}
