#include "proc/pid_namespace.h"
#include "proc/capabilities.h"
#include "internal.h"
#include "net/route.h"
#include "proc/domain.h"
#include "lib/mem.h"

struct nl_header { uint32_t len; uint16_t type, flags; uint32_t seq, port; };
struct address_msg { uint8_t family, prefix, flags, scope; uint32_t index; };
struct link_msg { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; };
struct attr { uint16_t len, type; };
enum { NL_ERROR = 2, NEWLINK = 16, NEWADDR = 20, ADDRESS = 1, LOCAL = 2 };

static struct open_file *bound;
static uint32_t next_port = UINT32_MAX;

static bool bound_port(const struct net_namespace *ns, uint32_t port) {
  for (struct open_file *it = bound; it; it = it->family_next) {
    if (it->net_ns == ns && it->route_pid == port) { return true; }
  }
  return false;
}

int route_bind(struct open_file *file, struct route_address address) {
  if (address.family != ROUTE_FAMILY || address.groups) { return -22; }
  if (file->route_pid) { return !address.port || address.port == file->route_pid ? 0 : -22; }
  uint32_t port = address.port;
  if (port) {
    if (bound_port(file->net_ns, port)) { return -98; }
  } else {
    port = pidns_self_view(cell_current_pid());
    while (!port || bound_port(file->net_ns, port)) { port = next_port--; }
  }
  file->route_pid = port;
  file->family_next = bound;
  bound = file;
  return 0;
}

void route_close(struct open_file *file) {
  if (file->route_pid) {
    struct open_file **link = &bound;
    while (*link != file) { link = &(*link)->family_next; }
    *link = file->family_next;
  }
  dgram_clear_rx(file);
}

static int loopback_address(const void *payload, size_t len, bool existing, unsigned flags) {
  struct address_msg msg;
  if (len < sizeof(msg)) { return -22; }
  kmemcpy(&msg, payload, sizeof(msg));
  if (msg.index != 1) { return -19; }
  if (msg.family != 2 || msg.prefix != 8 || msg.scope != 254) { return -95; }
  const uint8_t *p = (const uint8_t *)payload + sizeof(msg);
  len -= sizeof(msg);
  bool present = false;
  while (len) {
    struct attr a;
    if (len < sizeof(a)) { return -22; }
    kmemcpy(&a, p, sizeof(a));
    size_t step = ((size_t)a.len + 3) & ~(size_t)3;
    if (a.len < sizeof(a) || step > len) { return -22; }
    if (a.type != ADDRESS && a.type != LOCAL) { return -95; }
    uint32_t ip;
    if (a.len != sizeof(a) + sizeof(ip)) { return -22; }
    kmemcpy(&ip, p + sizeof(a), sizeof(ip));
    if (ip != 0x0100007f) { return -95; }
    present = true;
    p += step;
    len -= step;
  }
  if (!present) { return -22; }
  return existing && (flags & 0x200) ? -17 : 0;
}

int64_t route_send(struct open_file *file, const void *data, size_t len) {
  struct nl_header h;
  if (len < sizeof(h)) { return -22; }
  kmemcpy(&h, data, sizeof(h));
  if (h.len != len || !(h.flags & 1)) { return -22; }
  if (!file->route_pid) {
    int rc = route_bind(file, (struct route_address){.family = ROUTE_FAMILY});
    if (rc) { return rc; }
  }
  struct net_namespace *ns = file->net_ns;
  const uint8_t *body = (const uint8_t *)data + sizeof(h);
  size_t size = len - sizeof(h);
  int rc = -95;
  bool up = false;
  if (!ns || !cell_ns_capable(ns->owner, CAP_NET_ADMIN)) { rc = ns ? -1 : -95; }
  else if (h.type == NEWADDR) { rc = loopback_address(body, size, ns->lo_address, h.flags); }
  else if (h.type == NEWLINK && size == sizeof(struct link_msg)) {
    struct link_msg msg;
    kmemcpy(&msg, body, sizeof(msg));
    rc = msg.index != 1 ? -19 : msg.change != 1 ? -95 : 0;
    up = msg.flags & 1;
  }
  if (rc || (h.flags & 4)) {
    struct { struct nl_header header; int32_t error; struct nl_header request; } reply =
        {{sizeof(reply), NL_ERROR, 0x100, h.seq, file->route_pid}, rc, h};
    if (!dgram_push_rx(file, 0, 0, 0, &reply, sizeof(reply))) { return -105; }
  }
  if (!rc) {
    if (h.type == NEWLINK) { ns->lo_up = up; }
    else { ns->lo_address = true; }
  }
  cell_socket_wake_file(file);
  return len;
}
