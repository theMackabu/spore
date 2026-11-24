#include "abi/linux/shared/usercopy.h"
#include "internal.h"
#include "proc/thread.h"

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

int uring_copy_in(void *dst, uint64_t addr, size_t size) {
  return syscall_user_readable(addr, size) && vmm_copy_from_user(syscall_active_as(), dst, addr, size) ? 0 : -EFAULT;
}

static void clear_files(struct uring *r, bool notify) {
  struct uring_slot *files = r->files;
  unsigned nr = r->nr_files;
  r->files = NULL;
  r->nr_files = 0;
  for (unsigned i = 0; i < nr; ++i) {
    cell_release_open_file(files[i].file);
    uring_tag_put(notify ? r : NULL, files[i].tag);
  }
  table_free(files, nr, sizeof(*files));
}

void uring_drop_resources(struct uring *r) {
  clear_files(r, false);
  uring_drop_groups(r);
  uring_clear_buffers(r);
  cell_release_open_file(r->eventfd);
  r->eventfd = NULL;
}

int uring_update_files(struct uring *r, unsigned offset, uint64_t data, unsigned nr, uint64_t tags, bool update) {
  if (!nr || offset > r->nr_files || nr > r->nr_files - offset) { return -EINVAL; }
  if (!syscall_user_readable(data, nr * sizeof(int)) || (tags && !syscall_user_readable(tags, nr * sizeof(uint64_t)))) {
    return -EFAULT;
  }
  for (unsigned i = 0; i < nr; ++i) {
    int fd, error;
    uint64_t tag = 0;
    if (uring_copy_in(&fd, data + i * sizeof(fd), sizeof(fd)) ||
        (tags && uring_copy_in(&tag, tags + i * sizeof(tag), sizeof(tag)))) {
      error = -EFAULT;
      goto fail;
    }
    if (fd < 0 && tag) {
      error = -EINVAL;
      goto fail;
    }
    if (fd == -2 && update) { continue; }
    struct open_file *f = fd == -1 ? NULL : cell_fd_file(fd);
    if (fd != -1 && (!f || f->type == OPEN_URING)) {
      error = -EBADF;
      goto fail;
    }
    struct uring_extra *note = tag ? uring_extra_new(tag) : NULL;
    if (tag && !note) {
      error = -ENOMEM;
      goto fail;
    }
    cell_retain_open_file(f);
    struct uring_slot old = r->files[offset + i];
    r->files[offset + i] = (struct uring_slot){f, note};
    cell_release_open_file(old.file);
    uring_tag_put(r, old.tag);
    continue;
  fail:
    return i && update ? (int)i : error;
  }
  return nr;
}

static int register_files(struct uring *r, unsigned nr, uint64_t data, uint64_t tags, bool sparse) {
  if (r->files) { return -EBUSY; }
  if (!nr || nr > MAX_FDS || (sparse && tags)) { return -EINVAL; }
  r->files = table_alloc(nr, sizeof(*r->files));
  if (!r->files) { return -ENOMEM; }
  r->nr_files = nr;
  if (sparse) { return 0; }
  int n = uring_update_files(r, 0, data, nr, tags, false);
  if (n == (int)nr) { return 0; }
  clear_files(r, false);
  return n;
}

static int restrict_ring(struct uring *r, uint64_t addr, unsigned count) {
  struct rule {
    uint16_t type;
    uint8_t value, pad;
    uint32_t reserved[3];
  };
  if (!r->disabled) { return -77; }
  if (r->restricted) { return -16; }
  if (count > 256) { return -EINVAL; }
  if (!syscall_user_readable(addr, count * sizeof(struct rule))) { return -EFAULT; }
  uint64_t ops = 0, regs = 0;
  unsigned allowed = 0, required = 0;
  for (unsigned i = 0; i < count; ++i) {
    struct rule rule;
    if (uring_copy_in(&rule, addr + i * sizeof(rule), sizeof(rule))) { return -EFAULT; }
    switch (rule.type) {
    case 0:
      if (rule.value >= 64) { return -EINVAL; }
      regs |= 1ull << rule.value;
      break;
    case 1:
      if (rule.value >= 64) { return -EINVAL; }
      ops |= 1ull << rule.value;
      break;
    case 2:
      allowed = rule.value;
      break;
    case 3:
      required = rule.value;
      break;
    default:
      return -EINVAL;
    }
  }
  r->allowed_ops = ops;
  r->allowed_register = regs;
  r->allowed_flags = allowed | required;
  r->required_flags = required;
  r->restricted = true;
  return 0;
}

static int registration(struct open_file *f, uint32_t op, uint64_t addr, uint32_t count) {
  struct uring *r = &f->ring;
  if (!r->disabled && r->restricted && (op >= 64 || !(r->allowed_register & (1ull << op)))) { return -13; }
  switch (op) {
  case 22:
  case 23:
  case 26:
    return uring_register_group(r, op, addr, count);
  case 24: {
    struct {
      uint64_t key;
      int32_t fd;
      uint32_t flags;
      int64_t sec, nsec;
      uint8_t opcode, pad[7];
      uint64_t reserved[3];
    } cancel;
    if (count != 1) { return -EINVAL; }
    if (uring_copy_in(&cancel, addr, sizeof(cancel))) { return -EFAULT; }
    int rc = uring_cancel(r, cancel.key, cancel.fd, cancel.flags, cancel.opcode);
    uring_service_events();
    return rc;
  }
  case 11:
    return restrict_ring(r, addr, count);
  case 0:
    return uring_register_buffers(r, addr, count, 0, false);
  case 1:
    if (addr || count) { return -EINVAL; }
    if (!r->buffers) { return -6; }
    uring_clear_buffers(r);
    return 0;
  case REGISTER_PROBE:
    return probe(addr, count);
  case ENABLE_RINGS:
    if (!r->disabled || addr || count) { return -EINVAL; }
    if (r->issuer == -1) { r->issuer = cell_current_tid(); }
    r->disabled = false;
    return 0;
  case REG_EVENTFD:
  case REG_EVENTFD_ASYNC: {
    if (count != 1) { return -EINVAL; }
    if (r->eventfd) { return -EBUSY; }
    int n;
    if (uring_copy_in(&n, addr, sizeof(n))) { return -EFAULT; }
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
    return register_files(r, count, addr, 0, false);
  case UNREG_FILES:
    if (addr || count) { return -EINVAL; }
    if (!r->files) { return -6; }
    clear_files(r, true);
    return 0;
  case 15:
  case REG_FILES2: {
    struct resource_register reg;
    if (count != sizeof(reg)) { return -EINVAL; }
    if (uring_copy_in(&reg, addr, sizeof(reg))) { return -EFAULT; }
    if (reg.reserved || reg.flags & ~1u || ((reg.flags & 1) && reg.data)) { return -EINVAL; }

    return op == REG_FILES2 ? register_files(r, reg.nr, reg.data, reg.tags, reg.flags & 1)
                            : uring_register_buffers(r, reg.data, reg.nr, reg.tags, reg.flags & 1);
  }
  case FILES_UPDATE:
  case 16:
  case FILES_UPDATE2: {
    struct resource_update u = {0};
    if (op == 16 ? !r->buffers : !r->files) { return -6; }
    if (op != FILES_UPDATE && count != sizeof(u)) { return -EINVAL; }
    if (uring_copy_in(&u, addr, op == FILES_UPDATE ? 16 : sizeof(u))) { return -EFAULT; }
    if (u.reserved || u.reserved2) { return -EINVAL; }
    if (op == 16) { return uring_update_buffers(r, u.offset, u.data, u.nr, u.tags); }
    return uring_update_files(r, u.offset, u.data, op == FILES_UPDATE ? count : u.nr, u.tags, true);
  }
  default:
    return -EINVAL;
  }
}

int64_t sys_io_uring_register(int fd, uint32_t op, uint64_t addr, uint32_t count) {
  struct open_file *f = uring_file(fd);
  if (!f) { return -EBADF; }
  int rc = registration(f, op, addr, count);
  if (f->ring.overflow_head) {
    unsigned before = f->ring.cq_tail;
    uring_flush_overflow(&f->ring);
    if (before != f->ring.cq_tail) { uring_notify(f, false); }
  }
  return rc;
}

int uring_install_direct(struct uring *r, uint32_t index, struct open_file *target) {
  if (!r->files) { return -6; }
  bool allocate = index == UINT32_MAX;
  if (allocate) {
    for (index = 0; index < r->nr_files && r->files[index].file; ++index) {}
    if (index == r->nr_files) { return -23; }
  } else if (!index || --index >= r->nr_files) {
    return -EINVAL;
  }
  struct uring_slot old = r->files[index];
  cell_retain_open_file(target);
  r->files[index] = (struct uring_slot){.file = target};
  cell_release_open_file(old.file);
  uring_tag_put(r, old.tag);
  return allocate ? (int)index : 0;
}
int uring_direct_result(struct uring *r, uint32_t index, int fd) {
  if (!index || fd < 0) { return fd; }
  int rc = uring_install_direct(r, index, cell_fd_file(fd));
  cell_fd_close(fd);
  return rc;
}
int uring_close_direct(struct uring *r, uint32_t index) {
  if (!index || index > r->nr_files || !r->files[index - 1].file) { return -EBADF; }
  return uring_install_direct(r, index, NULL);
}
