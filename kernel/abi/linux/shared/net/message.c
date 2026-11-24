#include "abi/linux/shared/message.h"
#include "message_layout.h"
#include "proc/memory.h"

enum { SOCKET_MAX_IOVCNT = 1024, EINVAL = 22, EFAULT = 14, MSG_CTRUNC = 8, IPPROTO_IP = 0, IP_TTL = 2 };

int linux_read_socket_message(struct domain *domain, uint64_t addr, struct socket_user_message *out) {
  struct msghdr64 msg;
  if (!vmm_copy_from_user(cell_domain_as(domain), &msg, addr, sizeof(msg))) { return -EFAULT; }
  *out = (struct socket_user_message){
    .iov = msg.iov,
    .iovlen = msg.iovlen,
    .name = msg.name,
    .namelen_addr = addr + offsetof(struct msghdr64, namelen),
  };
  return 0;
}

int linux_write_msg_flags(struct domain *domain, uint64_t addr, int32_t flags) {
  return vmm_copy_to_user(cell_domain_as(domain), addr + offsetof(struct msghdr64, flags), &flags, sizeof(flags))
           ? 0
           : -EFAULT;
}

static bool read_bytes(struct linux_iov_cursor *cursor, void *dst, uint64_t addr, size_t size) {
  return vmm_copy_from_user(cursor->as, dst, addr, size) ||
         (cell_domain_ensure_user_range(cursor->domain, addr, size, VMM_ACCESS_READ) &&
          vmm_copy_from_user(cursor->as, dst, addr, size));
}

int linux_iov_init(struct linux_iov_cursor *cursor, struct domain *domain, uint64_t iov, int32_t count) {
  if (!domain || count < 0 || count > SOCKET_MAX_IOVCNT) { return -EINVAL; }
  uint64_t bytes = (uint64_t)count * sizeof(struct iovec64);
  if (bytes && (!iov || iov > UINT64_MAX - bytes)) { return -EFAULT; }
  *cursor = (struct linux_iov_cursor){.domain = domain, .as = cell_domain_as(domain), .next = iov, .end = iov + bytes};
  for (uint64_t addr = iov; addr < cursor->end;) {
    struct iovec64 batch[16];
    size_t n = cursor->end - addr < sizeof(batch) ? cursor->end - addr : sizeof(batch);
    if (!read_bytes(cursor, batch, addr, n)) { return -EFAULT; }
    for (size_t i = 0; i < n / sizeof(*batch); ++i) {
      if (batch[i].len > INT64_MAX - cursor->capacity) { return -EINVAL; }
      cursor->capacity += batch[i].len;
    }
    addr += n;
  }
  return 0;
}

int linux_iov_from_snapshot(struct linux_iov_cursor *cursor, struct domain *domain, const struct iovec64 *iov,
                            unsigned count) {
  *cursor = (struct linux_iov_cursor){
    .domain = domain, .as = cell_domain_as(domain), .snapshot = iov, .end = count * sizeof(*iov)};
  for (unsigned i = 0; i < count; ++i) {
    if (iov[i].len > INT64_MAX - cursor->capacity) { return -EINVAL; }
    cursor->capacity += iov[i].len;
  }
  return 0;
}

static size_t next_span(struct linux_iov_cursor *cursor, size_t want, uint64_t *addr) {
  while (!cursor->remaining) {
    struct iovec64 entry;
    if (cursor->next == cursor->end) { return 0; }
    if (cursor->snapshot) {
      entry = cursor->snapshot[cursor->next / sizeof(entry)];
    } else if (!read_bytes(cursor, &entry, cursor->next, sizeof(entry))) {
      return 0;
    }
    if (entry.len > UINT64_MAX - entry.base) { return 0; }
    cursor->addr = entry.base;
    cursor->remaining = entry.len;
    cursor->next += sizeof(entry);
  }
  size_t size = want < cursor->remaining ? want : cursor->remaining;
  *addr = cursor->addr;
  cursor->addr += size;
  cursor->remaining -= size;
  return size;
}

int linux_gather_iov(void *source, uint8_t *dst, size_t len) {
  struct linux_iov_cursor *cursor = source;
  while (len) {
    uint64_t addr;
    size_t n = next_span(cursor, len, &addr);
    if (!n || !read_bytes(cursor, dst, addr, n)) { return -EFAULT; }
    dst += n;
    len -= n;
  }
  return 0;
}

int linux_scatter_iov(void *destination, const uint8_t *src, size_t len) {
  struct linux_iov_cursor *cursor = destination;
  while (len) {
    uint64_t addr;
    size_t n = next_span(cursor, len, &addr);
    if (!n) { return -EFAULT; }
    if (!vmm_copy_to_user(cursor->as, addr, src, n) &&
        !(cell_domain_ensure_user_range(cursor->domain, addr, n, VMM_ACCESS_WRITE) &&
          vmm_copy_to_user(cursor->as, addr, src, n))) { return -EFAULT; }
    src += n;
    len -= n;
  }
  return 0;
}

int linux_recv_ip_control(struct domain *domain, uint64_t msg_addr, bool emit_ttl, int ttl_value, int32_t *flags) {
  struct msghdr64 msg;
  struct user_address_space *as = cell_domain_as(domain);
  if (!vmm_copy_from_user(as, &msg, msg_addr, sizeof(msg))) { return -EFAULT; }
  uint64_t used = 0;
  if (emit_ttl) {
    struct {
      uint64_t len;
      int32_t level, type, value, pad;
    } ttl = {20, IPPROTO_IP, IP_TTL, ttl_value, 0};
    if (msg.control && msg.controllen >= 16) {
      used = msg.controllen < sizeof(ttl) ? msg.controllen : sizeof(ttl);
      if (ttl.len > used) { ttl.len = used; }
      if (!vmm_copy_to_user(as, msg.control, &ttl, used)) { return -EFAULT; }
    }
    if (used < 20) { *flags |= MSG_CTRUNC; }
  }
  return vmm_copy_to_user(as, msg_addr + offsetof(struct msghdr64, controllen), &used, sizeof(used)) ? 0 : -EFAULT;
}
