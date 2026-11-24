#include "proc/uring.h"
#include "abi/linux/shared/open.h"
#include "abi/linux/shared/usercopy.h"
#include "handlers.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/epoll.h"
#include "proc/events/poll_internal.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "proc/poll.h"

enum {
  SETUP_CQSIZE = 1u << 3,
  SETUP_CLAMP = 1u << 4,
  SETUP_NO_SQARRAY = 1u << 16,
  SINGLE_MMAP = 1,
  GETEVENTS = 1,
  OFF_CQ_RING = 0x8000000,
  OFF_SQES = 0x10000000,
  OP_NOP = 0,
  OP_EPOLL_CTL = 29,
  REGISTER_PROBE = 8,
  MAX_ENTRIES = 1024,
  EBADF = 9,
  ENOMEM = 12,
  EFAULT = 14,
  EBUSY = 16,
  EINVAL = 22,
};

struct offsets {
  uint32_t head, tail, mask, entries, flags_or_overflow, dropped_or_cqes, array_or_flags, reserved;
  uint64_t addr;
};
struct params {
  uint32_t sq_entries, cq_entries, flags, cpu, idle, features, wq_fd, reserved[3];
  struct offsets sq, cq;
};
struct rings {
  uint32_t sq_head, sq_tail, sq_mask, sq_entries, sq_flags, dropped;
  uint32_t cq_head, cq_tail, cq_mask, cq_entries, overflow, cq_flags;
};
struct sqe {
  uint8_t opcode, flags;
  uint16_t priority;
  int32_t fd;
  uint64_t off, addr;
  uint32_t len, op_flags;
  uint64_t data;
  uint64_t extra[3];
};
struct cqe {
  uint64_t data;
  int32_t result;
  uint32_t flags;
};
static_assert(sizeof(struct params) == 120 && sizeof(struct sqe) == 64 && sizeof(struct cqe) == 16);

static uint32_t round_entries(uint32_t n) {
  uint32_t size = 1;
  while (size < n) {
    size <<= 1;
  }
  return size;
}

static unsigned pages(uint64_t len) {
  return (len + PAGE_SIZE - 1) / PAGE_SIZE;
}
static unsigned ring_pages(const struct uring *r) {
  return pages(sizeof(struct rings) + r->cq_entries * sizeof(struct cqe) + (r->sq_array_offset ? r->entries * 4 : 0));
}

static bool allocate(uint64_t *pa, unsigned n) {
  if (*pa) { return true; }
  *pa = pmm_alloc_contiguous_pages(n);
  if (!*pa) { return false; }
  kmemset(pmm_phys_to_virt(*pa), 0, n * PAGE_SIZE);
  return true;
}

void uring_release(struct uring *r) {
  if (r->rings) {
    for (unsigned i = 0; i < ring_pages(r); ++i) {
      pmm_free_page(r->rings + i * PAGE_SIZE);
    }
  }
  if (r->sqes) {
    for (unsigned i = 0; i < pages(r->entries * sizeof(struct sqe)); ++i) {
      pmm_free_page(r->sqes + i * PAGE_SIZE);
    }
  }
}

int64_t sys_io_uring_setup(uint32_t entries, uint64_t addr) {
  struct params p;
  struct user_address_space *as = syscall_active_as();
  if (!syscall_user_readable(addr, sizeof(p)) || !syscall_user_writable(addr, sizeof(p)) ||
      !vmm_copy_from_user(as, &p, addr, sizeof(p))) {
    return -EFAULT;
  }
  if (!entries || p.reserved[0] || p.reserved[1] || p.reserved[2] ||
      (p.flags & ~(SETUP_CQSIZE | SETUP_CLAMP | SETUP_NO_SQARRAY))) {
    return -EINVAL;
  }
  if (entries > MAX_ENTRIES && !(p.flags & SETUP_CLAMP)) { return -EINVAL; }
  uint32_t sq = round_entries(entries < MAX_ENTRIES ? entries : MAX_ENTRIES);
  uint32_t cq = 2 * sq;
  if (p.flags & SETUP_CQSIZE) {
    if (p.cq_entries < sq || (p.cq_entries > 2 * MAX_ENTRIES && !(p.flags & SETUP_CLAMP))) { return -EINVAL; }
    cq = round_entries(p.cq_entries < 2 * MAX_ENTRIES ? p.cq_entries : 2 * MAX_ENTRIES);
  }
  uint32_t array = p.flags & SETUP_NO_SQARRAY ? 0 : sizeof(struct rings) + cq * sizeof(struct cqe);
#define OFF(member) offsetof(struct rings, member)
  p = (struct params){
    .flags = p.flags,
    .sq_entries = sq,
    .cq_entries = cq,
    .features = SINGLE_MMAP,
    .sq = {OFF(sq_head), OFF(sq_tail), OFF(sq_mask), OFF(sq_entries), OFF(sq_flags), OFF(dropped), array, 0, 0},
    .cq = {OFF(cq_head), OFF(cq_tail), OFF(cq_mask), OFF(cq_entries), OFF(overflow), sizeof(struct rings),
           OFF(cq_flags), 0, 0}};
#undef OFF
  struct open_file *file = cell_alloc_open_file();
  if (!file) { return -ENOMEM; }
  file->type = OPEN_URING;
  file->flags = LINUX_O_RDWR;
  file->ring = (struct uring){.entries = sq, .cq_entries = cq, .sq_array_offset = array};
  int fd = cell_install_fd(cell_current_domain_internal(), file, true);
  if (fd >= 0 && !vmm_copy_to_user(as, addr, &p, sizeof(p))) {
    cell_fd_close(fd);
    return -EFAULT;
  }
  return fd;
}

int64_t uring_map(struct uring *r, uint64_t base, uint64_t size, uint64_t off, uint32_t prot, uint32_t flags) {
  bool sqes = off == OFF_SQES;
  if (!sqes && off != 0 && off != OFF_CQ_RING) { return -EINVAL; }
  unsigned n = sqes ? pages(r->entries * sizeof(struct sqe)) : ring_pages(r);
  if (size > (uint64_t)n * PAGE_SIZE || (prot & VMM_USER_EXEC)) { return -EINVAL; }
  uint64_t *backing = sqes ? &r->sqes : &r->rings;
  bool fresh = !*backing;
  if (!allocate(backing, n)) { return -ENOMEM; }
  if (fresh && !sqes) {
    struct rings *h = pmm_phys_to_virt(*backing);
    h->sq_mask = r->entries - 1;
    h->sq_entries = r->entries;
    h->cq_mask = r->cq_entries - 1;
    h->cq_entries = r->cq_entries;
  }
  if (!cell_add_vma_typed(base, base + size, prot, flags, VMA_RING)) { return -ENOMEM; }
  for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
    uint64_t pa = *backing + offset;
    if (pmm_share_page(pa)) {
      if (vmm_map_page(syscall_active_as(), base + offset, pa, prot | VMM_MAP_SHARED)) { continue; }
      pmm_free_page(pa);
    }
    cell_remove_vma(base, base + size);
    return -ENOMEM;
  }
  return base;
}

static uint32_t load(const uint32_t *v) {
  return __atomic_load_n(v, __ATOMIC_ACQUIRE);
}
static void publish(uint32_t *v, uint32_t n) {
  __atomic_store_n(v, n, __ATOMIC_RELEASE);
}

int uring_poll(struct uring *r, int events) {
  if (!r->rings) { return events & CELL_POLLOUT; }
  struct rings *h = pmm_phys_to_virt(r->rings);
  int ready = r->cq_tail != load(&h->cq_head) ? CELL_POLLIN : 0;
  if (load(&h->sq_tail) - r->sq_head < r->entries) { ready |= CELL_POLLOUT; }
  return ready & events;
}

static struct open_file *ring_file(int fd) {
  struct open_file *file = cell_fd_file(fd);
  return file && file->type == OPEN_URING ? file : NULL;
}

int64_t sys_io_uring_enter(struct trap_frame *frame, int fd, uint32_t submit, uint32_t minimum, uint32_t flags,
                           uint64_t sigmask, uint64_t sigsize, bool allow_epoll_ctl) {
  struct open_file *file = ring_file(fd);
  if (!file) { return -EBADF; }
  if (flags & ~GETEVENTS) { return -EINVAL; }
  uint64_t mask;
  if ((flags & GETEVENTS) && sigmask) {
    if (sigsize != sizeof(mask)) { return -EINVAL; }
    if (!syscall_user_readable(sigmask, sizeof(mask)) ||
        !vmm_copy_from_user(syscall_active_as(), &mask, sigmask, sizeof(mask))) {
      return -EFAULT;
    }
  }
  struct uring *r = &file->ring;
  if (!submit && (!(flags & GETEVENTS) || !minimum)) { return 0; }
  if (!r->rings || (submit && !r->sqes)) { return -EINVAL; }
  struct rings *h = pmm_phys_to_virt(r->rings);
  struct cqe *cq = (void *)(h + 1);
  struct sqe *sq = pmm_phys_to_virt(r->sqes);
  uint32_t pending = load(&h->sq_tail) - r->sq_head;
  if (pending > r->entries) { return -EINVAL; }
  if (submit > pending) { submit = pending; }
  uint32_t done = 0;
  while (done < submit && r->cq_tail - load(&h->cq_head) < r->cq_entries) {
    uint32_t i = r->sq_head & (r->entries - 1);
    if (r->sq_array_offset) { i = load((uint32_t *)((uint8_t *)h + r->sq_array_offset) + i); }
    ++r->sq_head;
    if (i >= r->entries) {
      publish(&h->dropped, ++r->dropped);
      break;
    }
    struct sqe e = sq[i];
    int result = -EINVAL;
    if (!e.flags && e.opcode == OP_NOP) { result = 0; }
    if (!e.flags && e.opcode == OP_EPOLL_CTL) {
      result = allow_epoll_ctl ? sys_epoll_ctl(e.fd, e.len, e.off, e.addr) : -1;
    }
    cq[r->cq_tail & (r->cq_entries - 1)] = (struct cqe){e.data, result, 0};
    publish(&h->cq_tail, ++r->cq_tail);
    ++done;
  }
  publish(&h->sq_head, r->sq_head);
  if (done) {
    size_t cursor = 0;
    struct thread *waiter;
    while ((waiter = cell_next_waiter(WAIT_URING, &cursor))) {
      if (waiter->wait.held_file == file && r->cq_tail - load(&h->cq_head) >= (unsigned)waiter->wait.target) {
        cell_finish_wait(waiter, waiter->wait.uring_submitted);
      }
    }
    cell_epoll_file_changed(file);
    cell_wake_poll_waiters_internal();
  } else if (submit && r->cq_tail - load(&h->cq_head) >= r->cq_entries) {
    return -EBUSY;
  }
  if (minimum > r->cq_entries) { minimum = r->cq_entries; }
  if ((flags & GETEVENTS) && minimum > r->cq_tail - load(&h->cq_head)) {
    struct thread *thread = cell_current_thread_internal();
    cell_retain_open_file(file);
    thread->wait =
      (struct thread_wait){.reason = WAIT_URING, .held_file = file, .target = minimum, .uring_submitted = done};
    return cell_sleep_poll(thread, frame, sigmask ? &mask : NULL, false, 0);
  }
  return done;
}

int64_t sys_io_uring_register(int fd, uint32_t op, uint64_t addr, uint32_t count) {
  if (!ring_file(fd)) { return -EBADF; }
  if (op != REGISTER_PROBE || count > 256) { return -EINVAL; }
  struct probe_op {
    uint8_t op, pad;
    uint16_t flags;
    uint32_t reserved;
  };
  struct {
    uint8_t last, len;
    uint8_t pad[14];
    struct probe_op ops[OP_EPOLL_CTL + 1];
  } probe = {0};
  probe.last = OP_EPOLL_CTL;
  probe.len = count < OP_EPOLL_CTL + 1 ? count : OP_EPOLL_CTL + 1;
  for (unsigned i = 0; i < probe.len; ++i) {
    probe.ops[i].op = i;
    probe.ops[i].flags = i == OP_NOP || i == OP_EPOLL_CTL;
  }
  size_t len = 16 + probe.len * sizeof(struct probe_op);
  return syscall_user_writable(addr, len) && vmm_copy_to_user(syscall_active_as(), addr, &probe, len) ? 0 : -EFAULT;
}
