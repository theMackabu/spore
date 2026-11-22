#include "elf/loader.h"

static bool vfs_elf_read_at(void *ctx, uint64_t offset, void *dst, size_t len) {
  const struct vfs_node *node = ctx;
  return node != NULL && vfs_read(node, offset, dst, len) == len;
}

bool elf_reader_from_vfs(const struct vfs_node *node, struct elf_reader *reader) {
  if (node == NULL || reader == NULL || node->is_dir || node->device != RAMFS_DEV_NONE) { return false; }
  *reader = (struct elf_reader){
    .read_at = vfs_elf_read_at,
    .ctx = (void *)node,
    .node = node,
    .size = node->size,
  };
  return true;
}
