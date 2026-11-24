#include "abi/linux/shared/usercopy.h"
#include "internal.h"

static int probe(uint64_t addr, uint32_t count) {
  if (count > 256) { return -EINVAL; }
  struct probe_op {
    uint8_t op, pad;
    uint16_t flags;
    uint32_t reserved;
  };
  struct {
    uint8_t last, len;
    uint8_t pad[14];
    struct probe_op ops[OP_LAST + 1];
  } probe = {0};
  probe.last = OP_LAST;
  probe.len = count < OP_LAST + 1 ? count : OP_LAST + 1;
  for (unsigned i = 0; i < probe.len; ++i) {
    probe.ops[i].op = i;
    probe.ops[i].flags = uring_opcode_supported(i);
  }
  size_t len = 16 + probe.len * sizeof(struct probe_op);
  return syscall_user_writable(addr, len) && vmm_copy_to_user(syscall_active_as(), addr, &probe, len) ? 0 : -EFAULT;
}

#include "mm/table.h"

enum {
  REG_FILES = 2,
  UNREG_FILES = 3,
  REG_EVENTFD = 4,
  UNREG_EVENTFD = 5,
  FILES_UPDATE = 6,
  REG_EVENTFD_ASYNC = 7,
  ENABLE_RINGS = 12,
  REG_FILES2 = 13,
  FILES_UPDATE2 = 14
};
struct resource_register {
  uint32_t nr, flags;
  uint64_t reserved, data, tags;
};
struct resource_update {
  uint32_t offset, reserved;
  uint64_t data, tags;
  uint32_t nr, reserved2;
};

static int copy_in(void *dst, uint64_t addr, size_t size) {
  return syscall_user_readable(addr, size) && vmm_copy_from_user(syscall_active_as(), dst, addr, size) ? 0 : -EFAULT;
}

static void clear_files(struct uring *r) {
  struct open_file **files = r->files;
  unsigned count = r->nr_files;
  r->files = NULL;
  r->nr_files = 0;
  for (unsigned i = 0; i < count; ++i) {
    cell_release_open_file(files[i]);
  }
  table_free(files, count, sizeof(*files));
}

void uring_drop_resources(struct uring *r) {
  clear_files(r);
  uring_clear_buffers(r);
  cell_release_open_file(r->eventfd);
  r->eventfd = NULL;
}

static int update_files(struct uring *r, unsigned offset, uint64_t addr, unsigned nr, bool update) {
  if (!nr || offset > r->nr_files || nr > r->nr_files - offset) { return -EINVAL; }
  if (!syscall_user_readable(addr, nr * sizeof(int))) { return -EFAULT; }
  for (unsigned i = 0; i < nr; ++i) {
    int fd;
    if (copy_in(&fd, addr + i * sizeof(fd), sizeof(fd))) { return i ? (int)i : -EFAULT; }
    if (update && fd == -2) { continue; }
    struct open_file *f = fd == -1 ? NULL : cell_fd_file(fd);
    if (fd != -1 && (!f || f->type == OPEN_URING)) { return i ? (int)i : -EBADF; }
    cell_retain_open_file(f);
    struct open_file *old = r->files[offset + i];
    r->files[offset + i] = f;
    cell_release_open_file(old);
  }
  return nr;
}

static int register_files(struct uring *r, unsigned nr, uint64_t data, bool sparse) {
  if (r->files) { return -EBUSY; }
  if (!nr || nr > MAX_FDS) { return -EINVAL; }
  r->files = table_alloc(nr, sizeof(*r->files));
  if (!r->files) { return -ENOMEM; }
  r->nr_files = nr;
  if (sparse) { return 0; }
  int n = update_files(r, 0, data, nr, false);
  if (n == (int)nr) { return 0; }
  clear_files(r);
  return n < 0 ? n : -EBADF;
}

int64_t sys_io_uring_register(int fd, uint32_t op, uint64_t addr, uint32_t count) {
  struct open_file *f = uring_file(fd);
  if (!f) { return -EBADF; }
  struct uring *r = &f->ring;
  switch (op) {
  case 0:
    return uring_register_buffers(r, addr, count);
  case 1:
    if (addr || count) { return -EINVAL; }
    if (!r->buffers) { return -6; }
    uring_clear_buffers(r);
    return 0;
  case REGISTER_PROBE:
    return probe(addr, count);
  case ENABLE_RINGS:
    if (!r->disabled || addr || count) { return -EINVAL; }
    r->disabled = false;
    return 0;
  case REG_EVENTFD:
  case REG_EVENTFD_ASYNC: {
    if (count != 1) { return -EINVAL; }
    if (r->eventfd) { return -EBUSY; }
    int n;
    if (copy_in(&n, addr, sizeof(n))) { return -EFAULT; }
    struct open_file *event = cell_fd_file(n);
    if (!event) { return -EBADF; }
    if (event->type != OPEN_EVENTFD) { return -EINVAL; }
    cell_retain_open_file(event);
    r->eventfd = event;
    r->eventfd_async = op == REG_EVENTFD_ASYNC;
    return 0;
  }
  case UNREG_EVENTFD:
    if (addr || count) { return -EINVAL; }
    if (!r->eventfd) { return -6; }
    cell_release_open_file(r->eventfd);
    r->eventfd = NULL;
    return 0;
  case REG_FILES:
    return register_files(r, count, addr, false);
  case UNREG_FILES:
    if (addr || count) { return -EINVAL; }
    if (!r->files) { return -6; }
    clear_files(r);
    return 0;
  case REG_FILES2: {
    struct resource_register reg;
    if (count != sizeof(reg)) { return -EINVAL; }
    if (copy_in(&reg, addr, sizeof(reg))) { return -EFAULT; }
    if (reg.reserved || reg.flags & ~1u || ((reg.flags & 1) && reg.data)) { return -EINVAL; }
    if (reg.tags) { return -95; }
    return register_files(r, reg.nr, reg.data, reg.flags & 1);
  }
  case FILES_UPDATE:
  case FILES_UPDATE2: {
    struct resource_update u = {0};
    if (!r->files) { return -6; }
    if (op == FILES_UPDATE2 && count != sizeof(u)) { return -EINVAL; }
    if (copy_in(&u, addr, op == FILES_UPDATE ? 16 : sizeof(u))) { return -EFAULT; }
    if (u.reserved || u.reserved2) { return -EINVAL; }
    if (u.tags) { return -95; }
    return update_files(r, u.offset, u.data, op == FILES_UPDATE ? count : u.nr, true);
  }
  default:
    return -EINVAL;
  }
}
