#include "abi/linux/shared/iovec.h"
#include "abi/linux/shared/numbers.h"
#include "abi/linux/shared/open.h"
#include "abi/linux/shared/usercopy.h"
#include "fs/notify.h"
#include "internal.h"
#include "mm/pmm.h"
#include "mm/pool.h"
#include "mm/table.h"
#include "proc/domain.h"
#include "proc/io.h"
#include "proc/policy.h"

struct uring_buffer {
  struct iovec64 iov;
  uint64_t *pages;
  struct uring_extra *tag;
  uint32_t nr, pinned, refs;
};

static struct object_pool buffers;

struct uring_buffer *uring_hold_buffer(struct uring *r, unsigned i) {
  struct uring_buffer *b = i < r->nr_buffers ? r->buffers[i] : NULL;
  if (b) { ++b->refs; }
  return b;
}

void uring_put_buffer(struct uring *r, struct uring_buffer *b, bool notify) {
  if (!b || --b->refs) { return; }
  for (unsigned j = 0; j < b->pinned; ++j) {
    pmm_unpin_page(b->pages[j]);
  }
  r->pinned_pages -= b->pinned;
  table_free(b->pages, b->nr, sizeof(*b->pages));
  uring_tag_put(notify ? r : NULL, b->tag);
  pool_free(&buffers, b);
}

static void clear_buffers(struct uring *r, bool notify) {
  for (unsigned i = 0; i < r->nr_buffers; ++i) {
    uring_put_buffer(r, r->buffers[i], notify);
  }
  table_free(r->buffers, r->nr_buffers, sizeof(*r->buffers));
  r->buffers = NULL;
  r->nr_buffers = 0;
}
void uring_clear_buffers(struct uring *r) {
  clear_buffers(r, true);
}

static int pin_buffer(struct uring *r, struct uring_buffer *b, uint64_t addr, uint64_t tagaddr, unsigned available) {
  uint64_t tag = 0;
  if (uring_copy_in(&b->iov, addr, sizeof(b->iov)) || (tagaddr && uring_copy_in(&tag, tagaddr, sizeof(tag)))) {
    return -EFAULT;
  }
  uint64_t base = b->iov.base, len = b->iov.len;
  if (!base && !len && !tag) { return 0; }
  if (!len || base >= USER_VA_LIMIT || len > USER_VA_LIMIT - base) { return -EFAULT; }
  uint64_t n = (base % PAGE_SIZE + len + PAGE_SIZE - 1) / PAGE_SIZE;
  if (n > available) { return -ENOMEM; }
  b->nr = n;
  b->pages = table_alloc(n, sizeof(*b->pages));
  if (!b->pages) { return -ENOMEM; }
  if (tag && !(b->tag = uring_extra_new(tag))) { return -ENOMEM; }
  for (unsigned j = 0; j < n; ++j) {
    uint64_t va = (base & ~(uint64_t)(PAGE_SIZE - 1)) + (uint64_t)j * PAGE_SIZE;
    if (!syscall_user_writable(va, 1) || !(b->pages[j] = vmm_pin_writable(syscall_active_as(), va))) { return -EFAULT; }
    ++b->pinned;
    ++r->pinned_pages;
  }
  return 0;
}
static int update(struct uring *r, unsigned offset, uint64_t addr, unsigned nr, uint64_t tags, bool partial) {
  if (!nr || offset > r->nr_buffers || nr > r->nr_buffers - offset) { return -EINVAL; }
  if (!syscall_user_readable(addr, nr * sizeof(struct iovec64)) ||
      (tags && !syscall_user_readable(tags, nr * sizeof(uint64_t)))) {
    return -EFAULT;
  }
  for (unsigned i = 0; i < nr; ++i) {
    struct uring_buffer *old = r->buffers[offset + i];
    if (!buffers.limit) { pool_init(&buffers, sizeof(struct uring_buffer), 65536); }
    struct uring_buffer *b = pool_alloc(&buffers);
    if (!b) { return i && partial ? (int)i : -ENOMEM; }
    b->refs = 1;
    unsigned available = 16384u - r->pinned_pages + (old && old->refs == 1 ? old->pinned : 0);
    int rc = pin_buffer(r, b, addr + i * sizeof(b->iov), tags ? tags + i * sizeof(uint64_t) : 0, available);
    if (rc) {
      uring_put_buffer(r, b, false);
      return i && partial ? (int)i : rc;
    }
    uring_put_buffer(r, old, true);
    r->buffers[offset + i] = b;
  }
  return nr;
}
int uring_update_buffers(struct uring *r, unsigned off, uint64_t addr, unsigned nr, uint64_t tags) {
  return update(r, off, addr, nr, tags, true);
}
int uring_register_buffers(struct uring *r, uint64_t addr, unsigned nr, uint64_t tags, bool sparse) {
  if (r->buffers) { return -EBUSY; }
  if (!nr || nr > 1024 || (sparse && tags)) { return -EINVAL; }
  r->buffers = table_alloc(nr, sizeof(*r->buffers));
  if (!r->buffers) { return -ENOMEM; }
  r->nr_buffers = nr;
  if (sparse) { return 0; }
  int n = update(r, 0, addr, nr, tags, false);
  if (n == (int)nr) { return 0; }
  clear_buffers(r, false);
  return n;
}

int uring_fixed_io(struct open_file *ring, const struct sqe *e) {
  bool write = e->opcode == OP_WRITE_FIXED;
  if (e->op_flags || e->len > INT32_MAX) { return -EINVAL; }
  if (e->off != UINT64_MAX && !cell_syscall_allowed(write ? SYS_PWRITE64 : SYS_PREAD64)) { return -1; }
  if (e->buf_index >= ring->ring.nr_buffers) { return -EFAULT; }
  struct uring_buffer *b = ring->ring.buffers[e->buf_index];
  if (!b || !b->pinned) { return -EFAULT; }
  struct open_file *f = uring_target(&ring->ring, e);
  if (!f) { return -EBADF; }
  if ((e->flags & IO_ASYNC) || uring_ordered(&ring->ring, e)) { return uring_defer_io(ring, f, e); }
  return uring_fixed_transfer(cell_current_domain_internal(), b, f, e);
}

int uring_fixed_transfer(struct domain *domain, struct uring_buffer *b, struct open_file *f, const struct sqe *e) {
  bool write = e->opcode == OP_WRITE_FIXED;
  if (e->addr < b->iov.base || e->addr - b->iov.base > b->iov.len || e->len > b->iov.len - (e->addr - b->iov.base)) {
    return -EFAULT;
  }
  if (!f || !(write ? linux_open_writable(f->flags) : linux_open_readable(f->flags))) { return -EBADF; }
  if (f->type != OPEN_VFS || f->node.device != RAMFS_DEV_NONE) { return -95; }
  if (f->node.is_dir) { return -21; }
  if (!vfs_refresh(&f->node, &f->node)) { return -5; }
  uint64_t off = e->off == UINT64_MAX ? f->offset : e->off;
  if (write && (f->flags & 02000)) { off = f->node.size; }
  if (off > INT64_MAX || e->len > INT64_MAX - off) { return -EINVAL; }
  size_t len = e->len;
  if (write) {
    int rc = cell_file_write_limit(domain, off, &len);
    if (rc) { return rc; }
  }
  unsigned done = 0;
  while (done < len) {
    uint64_t va = e->addr + done;
    unsigned page = va / PAGE_SIZE - b->iov.base / PAGE_SIZE;
    size_t n = PAGE_SIZE - va % PAGE_SIZE;
    while (n < len - done && page + 1 < b->nr && b->pages[page + 1] == b->pages[page] + PAGE_SIZE) {
      n += PAGE_SIZE;
      ++page;
    }
    if (n > len - done) { n = len - done; }
    uint8_t *data = (uint8_t *)pmm_phys_to_virt(b->pages[va / PAGE_SIZE - b->iov.base / PAGE_SIZE]) + va % PAGE_SIZE;
    int64_t result =
      write ? vfs_write(&f->node, off + done, data, n) : (int64_t)vfs_read(&f->node, off + done, data, n);
    if (result < 0) {
      if (!done) { return result; }
      break;
    }
    done += result;
    if ((size_t)result < n) { break; }
  }
  if (e->off == UINT64_MAX) { f->offset = off + done; }
  if (done) { fsnotify_file_event(f, write ? FS_MODIFY : FS_ACCESS); }
  return done;
}
