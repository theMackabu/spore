#include "internal.h"

int uring_message(struct open_file *source, const struct sqe *e) {
  struct open_file *dest = uring_file(e->fd);
  if (!dest || dest == source) { return -EBADF; }
  struct uring *r = &dest->ring;
  if (r->disabled) { return -77; }
  bool pass = e->addr == 1, skip = pass && (e->op_flags & 1);
  if (e->addr > 1 || (pass ? (e->len || e->op_flags & ~1u) : (e->op_flags & ~2u))) { return -EINVAL; }
  if (!uring_prepare_rings(r)) { return -ENOMEM; }
  struct uring_extra *item = skip ? NULL : uring_extra_new(e->off);
  if (!skip && !item) { return -ENOMEM; }
  int result = e->len;
  if (pass) {
    struct uring *from = &source->ring;
    result = -EBADF;
    if (e->addr3 < from->nr_files && from->files[e->addr3].file) {
      result = uring_install_direct(r, e->file_index, from->files[e->addr3].file);
    }
    if (result < 0) {
      if (item) { uring_extra_free(item); }
      return result;
    }
  }
  if (item) {
    item->cqe.result = result;
    item->cqe.flags = e->op_flags & 2 ? e->file_index : 0;
    uring_extra_append(r, item);
    uring_flush_overflow(r);
    uring_notify(dest, true);
  }
  return 0;
}
