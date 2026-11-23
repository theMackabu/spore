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
  DGRAM_RX_BUFFER_SLOTS = 64,
};

static uint8_t dgram_rx_buffers[DGRAM_RX_BUFFER_SLOTS][CELL_DGRAM_RX_CAP];

static bool dgram_rx_used[DGRAM_RX_BUFFER_SLOTS];

static uint16_t next_udp_port = 49152;

int dgram_poll_events(const struct open_file *file, int events) {
  int ready = file->socket_proto == IPPROTO_UDP && file->udp_error ? CELL_POLLERR : 0;
  if (events & CELL_POLLIN) {
    net_poll();
    if (file->dgram_rx_count) { ready |= CELL_POLLIN; }
  }
  return ready | (events & CELL_POLLOUT);
}

static int dgram_alloc_rx_slot(void) {
  for (size_t i = 0; i < DGRAM_RX_BUFFER_SLOTS; ++i) {
    if (dgram_rx_used[i]) { continue; }
    dgram_rx_used[i] = true;
    return (int)i;
  }
  return -1;
}

static void dgram_free_rx_slot(uint8_t slot) {
  if (slot < DGRAM_RX_BUFFER_SLOTS) { dgram_rx_used[slot] = false; }
}

static uint8_t dgram_queue_index(const struct open_file *file, uint8_t offset) {
  return (uint8_t)((file->dgram_rx_head + offset) % CELL_DGRAM_RX_QUEUE_CAP);
}

static uint32_t dgram_rx_queued_bytes(const struct open_file *file) {
  uint32_t total = 0;
  if (file == NULL) { return 0; }
  for (uint8_t i = 0; i < file->dgram_rx_count; ++i) {
    total += file->dgram_rx_len[dgram_queue_index(file, i)];
  }
  return total;
}

static void dgram_drop_head(struct open_file *file) {
  if (file == NULL || file->dgram_rx_count == 0) { return; }
  uint8_t index = file->dgram_rx_head;
  dgram_free_rx_slot(file->dgram_rx_slot[index]);
  file->dgram_rx_slot[index] = 0;
  file->dgram_rx_len[index] = 0;
  file->dgram_rx_ip[index] = 0;
  file->dgram_rx_port[index] = 0;
  file->dgram_rx_head = dgram_queue_index(file, 1);
  --file->dgram_rx_count;
  if (file->dgram_rx_count == 0) { file->dgram_rx_head = 0; }
}

void dgram_clear_rx(struct open_file *file) {
  if (file == NULL) { return; }
  while (file->dgram_rx_count != 0) {
    dgram_drop_head(file);
  }
}

static bool dgram_push_rx(struct open_file *file, uint32_t src_ip, uint8_t ttl, uint16_t src_port, const void *payload, size_t len) {
  if (file == NULL || file->dgram_rx_count >= CELL_DGRAM_RX_QUEUE_CAP) { return false; }
  if (len > CELL_DGRAM_RX_CAP) { len = CELL_DGRAM_RX_CAP; }
  uint32_t limit = socket_effective_buffer(file, true);
  uint32_t queued = dgram_rx_queued_bytes(file);
  if (queued >= limit || len > limit - queued) { return false; }
  int slot = dgram_alloc_rx_slot();
  if (slot < 0) { return false; }
  uint8_t index = dgram_queue_index(file, file->dgram_rx_count);
  file->dgram_rx_slot[index] = (uint8_t)slot;
  file->dgram_rx_len[index] = (uint16_t)len;
  file->dgram_rx_ip[index] = src_ip;
  file->dgram_rx_ttl[index] = ttl;
  file->dgram_rx_port[index] = src_port;
  kmemcpy(dgram_rx_buffers[slot], payload, len);
  ++file->dgram_rx_count;
  return true;
}

static struct open_file *udp_socket_for_fd(struct domain *domain, int fd) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL || domain->fds[fd]->type != OPEN_SOCKET) {
    return NULL;
  }
  return domain->fds[fd];
}

static bool udp_local_port_in_use(uint16_t port, const struct open_file *except) {
  if (port == 0) { return false; }
  for (size_t d = 0; d < cell_domain_capacity(); ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file != NULL && file != except && file->type == OPEN_SOCKET && file->udp_local_port == port) {
        return true;
      }
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
  file->udp_local_port = port;
  return 0;
}

bool cell_fd_udp_connect(int fd, uint32_t ip, uint16_t port) {
  struct open_file *file = udp_socket_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL || file->socket_proto != IPPROTO_UDP) { return false; }
  if (file->udp_local_port == 0) {
    uint16_t local_port = udp_ephemeral_port(file);
    if (local_port == 0) { return false; }
    file->udp_local_port = local_port;
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
    file->udp_local_port = local_port;
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

static bool dgram_copy_source_to_domain(struct domain *domain, uint32_t src_ip, uint16_t src_port, uint64_t addr,
                                        uint64_t addrlen) {
  if (addr == 0 && addrlen == 0) { return true; }
  if (addrlen != 0) {
    uint32_t len = sizeof(struct sockaddr_in_cell);
    if (!vmm_copy_to_user(cell_domain_as(domain), addrlen, &len, sizeof(len))) { return false; }
  }
  if (addr == 0) { return true; }
  struct sockaddr_in_cell sa;
  kmemset(&sa, 0, sizeof(sa));
  sa.sin_family = 2;
  sa.sin_port = net_bswap16(src_port);
  sa.sin_addr = src_ip;
  return vmm_copy_to_user(cell_domain_as(domain), addr, &sa, sizeof(sa));
}

int64_t dgram_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len, uint64_t addr,
                             uint64_t addrlen, uint32_t flags) {
  if (file != NULL && file->udp_error != 0) {
    int err = file->udp_error;
    file->udp_error = 0;
    return -(int64_t)err;
  }
  if (file == NULL || file->dgram_rx_count == 0) { return -EAGAIN; }
  uint8_t index = file->dgram_rx_head;
  uint8_t slot = file->dgram_rx_slot[index];
  uint64_t n = file->dgram_rx_len[index] < len ? file->dgram_rx_len[index] : len;
  if (!vmm_copy_to_user(cell_domain_as(domain), buf, dgram_rx_buffers[slot], (size_t)n)) { return -EFAULT; }
  if (!dgram_copy_source_to_domain(domain, file->dgram_rx_ip[index], file->dgram_rx_port[index], addr, addrlen)) {
    return -EFAULT;
  }
  if ((flags & MSG_PEEK) == 0) { dgram_drop_head(file); }
  return (int64_t)n;
}

int64_t dgram_read_to_kernel(struct domain *domain, struct open_file *file, void *buf, uint64_t len, uint64_t addr,
                             uint64_t addrlen, uint32_t flags, uint16_t *original_len) {
  if (file != NULL && file->udp_error != 0) {
    int err = file->udp_error;
    file->udp_error = 0;
    return -(int64_t)err;
  }
  if (file == NULL || file->dgram_rx_count == 0) { return -EAGAIN; }
  uint8_t index = file->dgram_rx_head;
  uint8_t slot = file->dgram_rx_slot[index];
  uint16_t packet_len = file->dgram_rx_len[index];
  if (original_len != NULL) { *original_len = packet_len; }
  uint64_t n = packet_len < len ? packet_len : len;
  if (n != 0) { kmemcpy(buf, dgram_rx_buffers[slot], (size_t)n); }
  if (!dgram_copy_source_to_domain(domain, file->dgram_rx_ip[index], file->dgram_rx_port[index], addr, addrlen)) {
    return -EFAULT;
  }
  if ((flags & MSG_PEEK) == 0) { dgram_drop_head(file); }
  return (int64_t)n;
}

static bool socket_matches_udp(struct open_file *file, uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_UDP &&
         file->udp_local_port == dst_port &&
         (!file->udp_connected || (file->udp_remote_ip == src_ip && file->udp_remote_port == src_port));
}

bool cell_net_deliver_udp(uint32_t src_ip, uint8_t ttl, uint16_t src_port, uint16_t dst_port, const void *payload, size_t len) {
  for (size_t d = 0; d < cell_domain_capacity(); ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (!socket_matches_udp(file, src_ip, src_port, dst_port)) { continue; }
      if (dgram_push_rx(file, src_ip, ttl, src_port, payload, len)) { cell_socket_wake_file(file); }
      return true;
    }
  }
  return false;
}

bool cell_net_deliver_udp_error(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port, int error) {
  for (size_t d = 0; d < cell_domain_capacity(); ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_UDP || !file->udp_connected) {
        continue;
      }
      if (file->udp_local_port != local_port || file->udp_remote_ip != remote_ip ||
          file->udp_remote_port != remote_port) {
        continue;
      }
      file->udp_error = (uint32_t)error;
      cell_socket_wake_file(file);
      return true;
    }
  }
  return false;
}

void cell_net_deliver_icmp(uint32_t src_ip, uint8_t ttl, const void *payload, size_t len) {
  if (len < 8) { return; }
  const uint8_t *icmp = payload;
  uint16_t id = (uint16_t)((icmp[4] << 8) | icmp[5]);
  for (size_t d = 0; d < cell_domain_capacity(); ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_ICMP ||
          file->udp_local_port == 0 || file->udp_local_port != id) { continue; }
      if (dgram_push_rx(file, src_ip, ttl, 0, payload, len)) { cell_socket_wake_file(file); }
      return;
    }
  }
}
