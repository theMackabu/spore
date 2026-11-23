#include "abi/linux/aarch64/message.h"
#include "message_layout.h"
#include "proc/memory.h"

enum { SOCKET_MAX_IOVCNT = 1024, EINVAL = 22, EFAULT = 14, MSG_CTRUNC = 8, IPPROTO_IP = 0, IP_TTL = 2 };

int aarch64_read_socket_message(struct domain *domain, uint64_t addr, struct socket_user_message *out) {
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

int aarch64_write_msg_flags(struct domain *domain, uint64_t addr, int32_t flags) {
  return vmm_copy_to_user(cell_domain_as(domain), addr + offsetof(struct msghdr64, flags), &flags, sizeof(flags))
           ? 0
           : -EFAULT;
}

static int socket_copy_iov_at(struct domain *domain, uint64_t iov_addr, int32_t iovlen, int32_t index,
                              struct iovec64 *out) {
  if (domain == NULL || out == NULL || iovlen < 0 || iovlen > SOCKET_MAX_IOVCNT || index < 0 || index >= iovlen) {
    return -EINVAL;
  }
  uint64_t addr = iov_addr + (uint64_t)index * sizeof(*out);
  return vmm_copy_from_user(cell_domain_as(domain), out, addr, sizeof(*out)) ? 0 : -EFAULT;
}

int aarch64_iov_capacity(struct domain *domain, uint64_t iov_addr, int32_t iovlen, uint64_t *capacity) {
  if (domain == NULL || capacity == NULL || iovlen < 0 || iovlen > SOCKET_MAX_IOVCNT) { return -EINVAL; }
  if (iovlen > 0 && iov_addr == 0) { return -EFAULT; }
  *capacity = 0;
  for (int32_t i = 0; i < iovlen; ++i) {
    struct iovec64 iov;
    int rc = socket_copy_iov_at(domain, iov_addr, iovlen, i, &iov);
    if (rc < 0) { return rc; }
    if (UINT64_MAX - *capacity < iov.len) { return -EINVAL; }
    *capacity += iov.len;
  }
  return 0;
}

int aarch64_gather_iov_at(struct domain *domain, uint64_t iov_addr, int32_t iovlen, uint64_t offset, uint8_t *dst,
                          size_t len) {
  if (domain == NULL || dst == NULL || iovlen < 0 || iovlen > SOCKET_MAX_IOVCNT) { return -EINVAL; }
  if (iovlen > 0 && iov_addr == 0) { return -EFAULT; }
  uint64_t base = 0;
  size_t done = 0;
  for (int32_t i = 0; i < iovlen && done < len; ++i) {
    struct iovec64 iov;
    int rc = socket_copy_iov_at(domain, iov_addr, iovlen, i, &iov);
    if (rc < 0) { return rc; }
    if (UINT64_MAX - base < iov.len) { return -EINVAL; }
    uint64_t end = base + iov.len;
    if (offset >= end) {
      base = end;
      continue;
    }
    uint64_t start = offset > base ? offset - base : 0;
    uint64_t available = iov.len - start;
    size_t n = (size_t)(available < (uint64_t)(len - done) ? available : (uint64_t)(len - done));
    if (n != 0 && !vmm_copy_from_user(cell_domain_as(domain), dst + done, iov.base + start, n)) { return -EFAULT; }
    done += n;
    base = end;
  }
  return done == len ? 0 : -EFAULT;
}

int aarch64_scatter_iov(struct domain *domain, uint64_t iov_addr, int32_t iovlen, const uint8_t *src, uint64_t len) {
  uint64_t done = 0;
  for (int32_t i = 0; i < iovlen && done < len; ++i) {
    struct iovec64 iov;
    int rc = socket_copy_iov_at(domain, iov_addr, iovlen, i, &iov);
    if (rc < 0) { return rc; }
    uint64_t remaining = len - done;
    size_t n = (size_t)(iov.len < remaining ? iov.len : remaining);
    if (n != 0 && !vmm_copy_to_user(cell_domain_as(domain), iov.base, src + done, n)) { return -EFAULT; }
    done += n;
  }
  return 0;
}

int aarch64_recv_ip_control(struct domain *domain, uint64_t msg_addr, bool emit_ttl, int ttl_value, int32_t *flags) {
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
