#include "internal.h"
#include "lib/mem.h"
#include "mm/pool.h"
#include "proc/memory.h"

static struct object_pool packets;
static_assert(CELL_DGRAM_RX_CAP * CELL_DGRAM_RX_QUEUE_CAP <= UINT16_MAX);

static void dgram_drop_head(struct open_file *file) {
  struct socket_datagram *packet = file->dgram_rx_head;
  file->dgram_rx_head = packet->next;
  if (!packet->next) { file->dgram_rx_tail = NULL; }
  file->dgram_rx_bytes -= packet->len;
  --file->dgram_rx_count;
  if (!file->dgram_rx_spare) { file->dgram_rx_spare = packet; }
  else { pool_free(&packets, packet); }
}

void dgram_clear_rx(struct open_file *file) {
  while (file->dgram_rx_head) { dgram_drop_head(file); }
  if (file->dgram_rx_spare) { pool_free(&packets, file->dgram_rx_spare); }
  file->dgram_rx_spare = NULL;
}

bool dgram_push_rx(struct open_file *file, uint32_t ip, uint8_t ttl, uint16_t port, const void *data, size_t len) {
  if (file->dgram_rx_count >= CELL_DGRAM_RX_QUEUE_CAP) { return false; }
  if (len > CELL_DGRAM_RX_CAP) { len = CELL_DGRAM_RX_CAP; }
  uint32_t limit = socket_effective_buffer(file, true);
  if (file->dgram_rx_bytes >= limit || len > limit - file->dgram_rx_bytes) { return false; }
  struct socket_datagram *packet = file->dgram_rx_spare;
  if (packet) { file->dgram_rx_spare = NULL; }
  else {
    if (!packets.limit && !pool_init(&packets, sizeof(*packet), SIZE_MAX)) { return false; }
    packet = pool_alloc(&packets);
    if (!packet) { return false; }
  }
  packet->next = NULL;
  packet->ip = ip;
  packet->ttl = ttl;
  packet->port = port;
  packet->len = len;
  kmemcpy(packet->data, data, len);
  if (file->dgram_rx_tail) { file->dgram_rx_tail->next = packet; }
  else { file->dgram_rx_head = packet; }
  file->dgram_rx_tail = packet;
  file->dgram_rx_bytes += len;
  ++file->dgram_rx_count;
  return true;
}

static bool dgram_copy_source_to_domain(struct domain *domain, uint32_t src_ip, uint16_t src_port, uint64_t addr,
                                        uint64_t addrlen) {
  if (!addr) { return true; }
  struct user_address_space *as = cell_domain_as(domain);
  uint32_t size;
  if (!addrlen || !vmm_copy_from_user(as, &size, addrlen, sizeof(size))) { return false; }
  struct sockaddr_in_cell sa = {.sin_family = 2, .sin_port = net_bswap16(src_port), .sin_addr = src_ip};
  if (size > sizeof(sa)) { size = sizeof(sa); }
  uint32_t actual = sizeof(sa);
  return vmm_copy_to_user(as, addr, &sa, size) && vmm_copy_to_user(as, addrlen, &actual, sizeof(actual));
}

int64_t dgram_read(struct domain *domain, struct open_file *file, uint64_t len, uint64_t addr, uint64_t addrlen,
                   uint32_t flags, socket_copy_out copy, void *destination) {
  if (file->udp_error) {
    int error = file->udp_error;
    file->udp_error = 0;
    return -error;
  }
  struct socket_datagram *packet = file->dgram_rx_head;
  if (!packet) { return -EAGAIN; }
  uint16_t packet_len = packet->len;
  size_t bytes = packet_len < len ? packet_len : len;
  int rc = copy(destination, packet->data, bytes);
  if (rc) { return rc; }
  if (!dgram_copy_source_to_domain(domain, packet->ip, packet->port, addr, addrlen)) { return -EFAULT; }
  if (!(flags & MSG_PEEK)) { dgram_drop_head(file); }
  return flags & MSG_TRUNC ? packet_len : (int64_t)bytes;
}
