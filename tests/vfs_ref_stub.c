#include "vfs.h"

static unsigned retain_count;
static unsigned release_count;
static unsigned shared_write_count;

void vfs_ref_stub_reset(void) {
  retain_count = 0;
  release_count = 0;
  shared_write_count = 0;
}

unsigned vfs_ref_stub_retain_count(void) {
  return retain_count;
}

unsigned vfs_ref_stub_release_count(void) {
  return release_count;
}

unsigned vfs_ref_stub_shared_write_count(void) {
  return shared_write_count;
}

void vfs_retain_node(const struct vfs_node *node) {
  (void)node;
  ++retain_count;
}

void vfs_release_node(const struct vfs_node *node) {
  (void)node;
  ++release_count;
}

void vfs_note_shared_writable_mapping(const struct vfs_node *node, bool add) {
  (void)node;
  if (add) {
    ++shared_write_count;
  } else if (shared_write_count != 0) {
    --shared_write_count;
  }
}
