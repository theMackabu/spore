#include "../usercopy.h"
#include "handlers.h"
#include "net/net.h"
#include "proc/capabilities.h"
#include "proc/domain.h"

enum { EPERM = 1, EFAULT = 14, EINVAL = 22 };

struct net_config64 {
  uint32_t local_ip;
  uint32_t gateway_ip;
  uint32_t netmask;
  uint32_t dns_ip;
  uint32_t configured;
};

int64_t sys_spore_net_config(uint64_t op, uint64_t cfg_addr) {
  if (cfg_addr == 0) { return -(int64_t)EFAULT; }
  if (op == 0) {
    struct net_config cfg;
    if (cell_current_domain_internal()->net_ns) {
      cfg = (struct net_config){0};
    } else {
      net_get_config(&cfg);
    }
    struct net_config64 out = {
      .local_ip = cfg.local_ip,
      .gateway_ip = cfg.gateway_ip,
      .netmask = cfg.netmask,
      .dns_ip = cfg.dns_ip,
      .configured = cfg.configured,
    };
    return syscall_user_writable(cfg_addr, sizeof(out)) &&
               vmm_copy_to_user(syscall_active_as(), cfg_addr, &out, sizeof(out))
             ? 0
             : -(int64_t)EFAULT;
  }
  if (op == 1) {
    if (cell_current_domain_internal()->net_ns || !cell_capable(CAP_NET_ADMIN)) { return -(int64_t)EPERM; }
    struct net_config64 in;
    if (!syscall_user_readable(cfg_addr, sizeof(in)) ||
        !vmm_copy_from_user(syscall_active_as(), &in, cfg_addr, sizeof(in))) {
      return -(int64_t)EFAULT;
    }
    struct net_config cfg = {
      .local_ip = in.local_ip,
      .gateway_ip = in.gateway_ip,
      .netmask = in.netmask,
      .dns_ip = in.dns_ip,
      .configured = in.configured != 0,
    };
    net_set_config(&cfg);
    return 0;
  }
  return -(int64_t)EINVAL;
}
