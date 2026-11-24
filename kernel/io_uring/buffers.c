#include "abi/linux/shared/iovec.h"
#include "abi/linux/shared/numbers.h"
#include "abi/linux/shared/open.h"
#include "abi/linux/shared/usercopy.h"
#include "fs/notify.h"
#include "internal.h"
#include "mm/pmm.h"
#include "mm/table.h"
#include "proc/policy.h"

struct uring_buffer {
  struct iovec64 iov;
  uint64_t *pages;
  uint32_t nr, pinned;
};

void uring_clear_buffers(struct uring *r) {
  for (unsigned i = 0; i < r->nr_buffers; ++i) {
    struct uring_buffer *b = &r->buffers[i];
    for (unsigned j = 0; j < b->pinned; ++j) {
      pmm_unpin_page(b->pages[j]);
    }
    table_free(b->pages, b->nr, sizeof(*b->pages));
  }
  table_free(r->buffers, r->nr_buffers, sizeof(*r->buffers));
  r->buffers = NULL;
  r->nr_buffers = 0;
}

int uring_register_buffers(struct uring *r, uint64_t addr, unsigned nr) {
  if (r->buffers) { return -EBUSY; }
  if (!nr || nr > 1024) { return -EINVAL; }
  if (!syscall_user_readable(addr, nr * sizeof(struct iovec64))) { return -EFAULT; }
  r->buffers = table_alloc(nr, sizeof(*r->buffers));
  if (!r->buffers) { return -ENOMEM; }
  r->nr_buffers = nr;
  unsigned total = 0;
  int error = -EFAULT;
  struct user_address_space *as = syscall_active_as();
  for (unsigned i = 0; i < nr; ++i) {
    struct uring_buffer *b = &r->buffers[i];
    if (!vmm_copy_from_user(as, &b->iov, addr + i * sizeof(b->iov), sizeof(b->iov))) { goto fail; }
    uint64_t base = b->iov.base, len = b->iov.len;
    if (!len || base >= USER_VA_LIMIT || len > USER_VA_LIMIT - base) { goto fail; }
    uint64_t n = (base % PAGE_SIZE + len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (n > 16384u - total) {
      error = -ENOMEM;
      goto fail;
    }
    total += n;
    b->nr = n;
    b->pages = table_alloc(n, sizeof(*b->pages));
    if (!b->pages) {
      error = -ENOMEM;
      goto fail;
    }
    for (unsigned j = 0; j < n; ++j) {
      uint64_t va = (base & ~(uint64_t)(PAGE_SIZE - 1)) + (uint64_t)j * PAGE_SIZE;
      if (!syscall_user_writable(va, 1)) { goto fail; }
      b->pages[j] = vmm_pin_writable(as, va);
      if (!b->pages[j]) { goto fail; }
      ++b->pinned;
    }
  }
  return 0;
fail:
  uring_clear_buffers(r);
  return error;
}

int uring_fixed_io(struct open_file *ring, const struct sqe *e) {
  bool write = e->opcode == OP_WRITE_FIXED;
  if (e->op_flags || e->len > INT32_MAX) { return -EINVAL; }
  if (e->off != UINT64_MAX && !cell_syscall_allowed(write ? SYS_PWRITE64 : SYS_PREAD64)) { return -1; }
  if (e->buf_index >= ring->ring.nr_buffers) { return -EFAULT; }
  struct uring_buffer *b = &ring->ring.buffers[e->buf_index];
  if (e->addr < b->iov.base || e->addr - b->iov.base > b->iov.len || e->len > b->iov.len - (e->addr - b->iov.base)) {
    return -EFAULT;
  }
  struct open_file *f = uring_target(&ring->ring, e);
  if (!f || !(write ? linux_open_writable(f->flags) : linux_open_readable(f->flags))) { return -EBADF; }
  if (f->type != OPEN_VFS || f->node.device != RAMFS_DEV_NONE) { return -95; }
  if (f->node.is_dir) { return -21; }
  if (!vfs_refresh(&f->node, &f->node)) { return -5; }
  uint64_t off = e->off == UINT64_MAX ? f->offset : e->off;
  if (write && (f->flags & 02000)) { off = f->node.size; }
  if (off > INT64_MAX || e->len > INT64_MAX - off) { return -EINVAL; }
  unsigned done = 0;
  while (done < e->len) {
    uint64_t va = e->addr + done;
    unsigned page = va / PAGE_SIZE - b->iov.base / PAGE_SIZE;
    size_t n = PAGE_SIZE - va % PAGE_SIZE;
    while (n < e->len - done && page + 1 < b->nr && b->pages[page + 1] == b->pages[page] + PAGE_SIZE) {
      n += PAGE_SIZE;
      ++page;
    }
    if (n > e->len - done) { n = e->len - done; }
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
