#include "internal.h"
#include "abi/linux/aarch64/message.h"
#include "net/net.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/thread.h"

struct socket_user_buffer {
  struct user_address_space *as;
  uint64_t addr;
};

static int copy_user_payload(void *source, uint8_t *dst, size_t len) {
  struct socket_user_buffer *buf = source;
  if (!vmm_copy_from_user(buf->as, dst, buf->addr, len)) { return -EFAULT; }
  buf->addr += len;
  return 0;
}

int64_t cell_socket_tcp_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  struct socket_user_buffer source = {cell_domain_as(domain), buf};
  return source.as ? tcp_write_stream(file, len, copy_user_payload, &source) : -EFAULT;
}

static int copy_received_payload(void *destination, const uint8_t *src, size_t len) {
  struct socket_user_buffer *buf = destination;
  if (!vmm_copy_to_user(buf->as, buf->addr, src, len)) { return -EFAULT; }
  buf->addr += len;
  return 0;
}

int64_t cell_socket_tcp_read_to_domain_flags(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len,
                                            uint32_t flags) {
  struct socket_user_buffer dst = {cell_domain_as(domain), buf};
  return dst.as ? tcp_read_stream(file, len, flags, copy_received_payload, &dst) : -EFAULT;
}

int64_t dgram_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len, uint64_t addr,
                             uint64_t addrlen, uint32_t flags) {
  struct socket_user_buffer dst = {cell_domain_as(domain), buf};
  return dst.as ? dgram_read(domain, file, len, addr, addrlen, flags, copy_received_payload, &dst) : -EFAULT;
}

int64_t socket_recvmsg_to_iov(struct domain *domain, struct open_file *file, uint64_t msg_addr, uint64_t iov,
                              int32_t iovlen, uint64_t addr, uint64_t addrlen, uint32_t flags) {
  struct aarch64_iov_cursor cursor;
  int rc = aarch64_iov_init(&cursor, domain, iov, iovlen);
  if (rc) { return rc; }
  if (!msg_addr && !cursor.capacity) { return 0; }
  bool tcp = file->socket_proto == IPPROTO_TCP;
  if (tcp && (tcp_recv_waitall_pending(file, cursor.capacity, flags) ||
              (cursor.capacity && !tcp_rx_available(file) && !file->tcp_fin && !file->tcp_error))) { return -EAGAIN; }
  int32_t msg_flags = 0;
  if (!tcp && !file->udp_error && !file->dgram_rx_count) { return -EAGAIN; }
  if (msg_addr && (tcp || !file->udp_error)) {
    const struct socket_datagram *packet = tcp ? NULL : file->dgram_rx_head;
    bool truncated = packet && packet->len > cursor.capacity;
    msg_flags = truncated ? MSG_TRUNC : 0;
    if (aarch64_recv_ip_control(domain, msg_addr, !tcp && file->ip_recvttl,
                                packet ? packet->ttl : 0, &msg_flags) < 0 ||
        aarch64_write_msg_flags(domain, msg_addr, msg_flags) < 0) { return -EFAULT; }
  }
  if (tcp) { return tcp_read_stream(file, cursor.capacity, flags, aarch64_scatter_iov, &cursor); }
  return dgram_read(domain, file, cursor.capacity, addr, addrlen, flags, aarch64_scatter_iov, &cursor);
}

int64_t cell_fd_dgram_writev(int fd, uint64_t iov, int32_t count) {
  struct domain *domain = cell_current_domain_internal();
  uint8_t packet[CELL_DGRAM_RX_CAP];
  struct aarch64_iov_cursor cursor;
  int rc = aarch64_iov_init(&cursor, domain, iov, count);
  if (rc < 0) { return rc; }
  uint64_t len = cursor.capacity;
  if (len > sizeof(packet)) { return -90; }
  rc = aarch64_gather_iov(&cursor, packet, (size_t)len);
  if (rc < 0) { return rc; }
  return cell_fd_udp_send_kernel(fd, 0, 0, packet, len);
}

int64_t cell_fd_dgram_readv(int fd, uint64_t iov, int32_t count, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = domain->fds[fd];
  net_poll();
  int64_t got = socket_recvmsg_to_iov(domain, file, 0, iov, count, 0, 0, 0);
  if (got != -EAGAIN || (file->flags & CELL_O_NONBLOCK) || frame == NULL) { return got; }
  return cell_block_current_on_socket_msg_timeout(fd, 0, iov, count, 0, 0, 0, file->so_rcvtimeo_ticks, frame);
}

int64_t socket_sendmsg_tcp_from_domain(struct domain *domain, struct open_file *file, uint64_t msg_addr) {
  struct socket_user_message msg;
  if (aarch64_read_socket_message(domain, msg_addr, &msg) < 0) { return -EFAULT; }
  struct aarch64_iov_cursor cursor;
  int rc = aarch64_iov_init(&cursor, domain, msg.iov, msg.iovlen);
  if (rc || !cursor.capacity) { return rc; }
  return tcp_write_stream(file, cursor.capacity, aarch64_gather_iov, &cursor);
}

int64_t cell_fd_socket_sendmsg(int fd, uint64_t msg_addr, bool dontwait, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  net_poll();
  int64_t sent = socket_sendmsg_tcp_from_domain(domain, file, msg_addr);
  if (sent != -EAGAIN || dontwait || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return sent; }
  if (!tcp_send_would_block(file)) { return sent; }
  return cell_block_current_on_socket_sendmsg_timeout(fd, msg_addr, file->so_sndtimeo_ticks, frame);
}

int64_t cell_fd_socket_recvmsg(int fd, uint64_t msg_addr, uint32_t flags, bool dontwait, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET) { return -9; }
  struct socket_user_message msg;
  if (aarch64_read_socket_message(domain, msg_addr, &msg) < 0) { return -EFAULT; }
  net_poll();
  bool can_block = !dontwait && (file->flags & CELL_O_NONBLOCK) == 0 && frame != NULL;
  uint32_t recv_flags = can_block ? flags : (flags & ~((uint32_t)MSG_WAITALL));
  int64_t got = socket_recvmsg_to_iov(domain, file, msg_addr, msg.iov, msg.iovlen, msg.name, msg.namelen_addr, recv_flags);
  if (got != -EAGAIN || !can_block) { return got; }
  return cell_block_current_on_socket_msg_timeout(fd, msg_addr, msg.iov, (uint64_t)msg.iovlen, msg.name, msg.namelen_addr,
                                                  flags, file->so_rcvtimeo_ticks, frame);
}
