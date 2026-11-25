#include "internal.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "net/unix.h"
#include "proc/domain.h"

static struct open_file *sockets;
static uint64_t next_id;

int unix_dgram_init(struct open_file *file) {
  if (next_id == UINT64_MAX) { return -28; }
  file->unix_dgram_id = ++next_id;
  file->family_next = sockets;
  sockets = file;
  return 0;
}

void unix_dgram_close(struct open_file *file) {
  if (file->unix_dgram_id) {
    struct open_file **link = &sockets;
    while (*link != file) {
      link = &(*link)->family_next;
    }
    *link = file->family_next;
  }
  if (file->node.ino) { vfs_release_node(&file->node); }
  dgram_clear_rx(file);
}

int unix_dgram_bind(struct open_file *file, const char *path, const char *name) {
  if (file->node.ino) { return -EINVAL; }
  struct vfs_node node;
  int rc = vfs_mkspecial(path, 0140000 | cell_creation_mode(0777), &node);
  if (rc) { return rc == -17 ? -EADDRINUSE : rc; }
  (void)vfs_chown_node(&node, cell_current_euid(), cell_current_egid());
  if (!vfs_retain_node(&node)) {
    (void)vfs_remove(path);
    return -12;
  }
  file->node = node;
  copy_cstr(file->path, sizeof(file->path), name);
  return 0;
}

static struct open_file *destination(const struct open_file *file, const struct vfs_node *node) {
  for (struct open_file *p = sockets; p; p = p->family_next) {
    if (node ? p->node.ino == node->ino && p->node.dev_id == node->dev_id : file->unix_dgram_peer == p->unix_dgram_id) {
      return p;
    }
  }
  return NULL;
}

const char *unix_dgram_name(const struct open_file *file, bool peer) {
  if (peer) { file = destination(file, NULL); }
  return file ? file->path : NULL;
}

int unix_dgram_connect(struct open_file *file, const struct vfs_node *node) {
  struct open_file *peer = destination(file, node);
  if (!peer) { return -ECONNREFUSED; }
  file->unix_dgram_peer = peer->unix_dgram_id;
  return 0;
}

int64_t unix_dgram_send(struct open_file *file, const struct vfs_node *node, const void *data, size_t len) {
  if (!node && !file->unix_dgram_peer) { return -107; }
  struct open_file *peer = destination(file, node);
  if (!peer) { return -ECONNREFUSED; }
  if (peer->unix_dgram_peer && peer->unix_dgram_peer != file->unix_dgram_id) { return -EPERM; }
  if (len > CELL_DGRAM_RX_CAP) { return -90; }
  if (!dgram_push_from(peer, 0, 0, 0, file->path, data, len)) { return -EAGAIN; }
  cell_socket_wake_file(peer);
  return len;
}
