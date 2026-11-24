#include "abi/linux/shared/usercopy.h"
#include "internal.h"
#include "mm/pmm.h"
#include "mm/pool.h"
#include "mm/table.h"

struct extent {
  struct extent *next;
  uint64_t addr;
  uint32_t len, count, bid;
};
struct uring_group {
  struct uring_group *next;
  uint16_t id, head;
  uint32_t size, available;
  struct extent *first, *last;
  uint64_t *pages;
};
static struct object_pool groups, extents;

static struct uring_group **find(struct uring *r, unsigned id) {
  struct uring_group **slot = &r->groups;
  while (*slot && (*slot)->id != id) {
    slot = &(*slot)->next;
  }
  return slot;
}
static struct uring_group *create(struct uring_group **slot, unsigned id) {
  if (!groups.limit) { pool_init(&groups, sizeof(struct uring_group), 65536); }
  *slot = pool_alloc(&groups);
  if (*slot) { (*slot)->id = id; }
  return *slot;
}
static unsigned page_count(unsigned size) {
  return (size * 16u + PAGE_SIZE - 1) / PAGE_SIZE;
}
static void drop(struct uring *r, struct uring_group **slot) {
  struct uring_group *g = *slot;
  *slot = g->next;
  while (g->first) {
    struct extent *item = g->first;
    g->first = item->next;
    pool_free(&extents, item);
  }
  if (g->pages) {
    unsigned count = page_count(g->size);
    for (unsigned i = 0; i < count && g->pages[i]; ++i) {
      pmm_unpin_page(g->pages[i]);
      --r->pinned_pages;
    }
    table_free(g->pages, count, sizeof(*g->pages));
  }
  pool_free(&groups, g);
}
void uring_drop_groups(struct uring *r) {
  while (r->groups) {
    drop(r, &r->groups);
  }
}

static void consume_extent(struct uring_group *g, unsigned n) {
  struct extent *x = g->first;
  g->available -= n;
  x->addr += (uint64_t)x->len * n;
  x->bid += n;
  x->count -= n;
  if (!x->count) {
    g->first = x->next;
    if (!g->first) { g->last = NULL; }
    pool_free(&extents, x);
  }
}
int uring_group_op(struct uring *r, const struct sqe *e) {
  if (e->fd <= 0 || e->fd > 65536 || e->op_flags) { return -EINVAL; }
  struct uring_group **slot = find(r, e->buf_index), *g = *slot;
  if (g && g->size) { return -EINVAL; }
  if (e->opcode == OP_REMOVE_BUFFERS) {
    if (!g) { return -2; }
    unsigned removed = 0;
    while (g->first && removed < (unsigned)e->fd) {
      unsigned n = e->fd - removed;
      if (n > g->first->count) { n = g->first->count; }
      consume_extent(g, n);
      removed += n;
    }
    if (!g->first) { drop(r, slot); }
    return removed;
  }
  if (!e->len || e->off > 65535 || e->off + e->fd > 65536 || e->addr >= USER_VA_LIMIT ||
      (uint64_t)e->len * e->fd > USER_VA_LIMIT - e->addr) {
    return -EINVAL;
  }
  if (g && g->available > 65536u - e->fd) { return -7; }
  if (!extents.limit) { pool_init(&extents, sizeof(struct extent), 65536); }
  struct extent *x = pool_alloc(&extents);
  if (!x) { return -ENOMEM; }
  if (!g && !(g = create(slot, e->buf_index))) {
    pool_free(&extents, x);
    return -ENOMEM;
  }
  *x = (struct extent){.addr = e->addr, .len = e->len, .count = e->fd, .bid = e->off};
  if (g->last) {
    g->last->next = x;
  } else {
    g->first = x;
  }
  g->last = x;
  g->available += e->fd;
  return 0;
}

int uring_register_group(struct uring *r, unsigned op, uint64_t addr, unsigned count) {
  if (count != 1) { return -EINVAL; }
  if (op == 26) {
    struct {
      uint32_t id, head, reserved[8];
    } status;
    if (uring_copy_in(&status, addr, sizeof(status))) { return -EFAULT; }
    struct uring_group *g = *find(r, status.id);
    if (!g || !g->size) { return -2; }
    status.head = g->head;
    return syscall_user_writable(addr, sizeof(status)) &&
               vmm_copy_to_user(syscall_active_as(), addr, &status, sizeof(status))
             ? 0
             : -EFAULT;
  }
  struct {
    uint64_t addr;
    uint32_t entries;
    uint16_t id, flags;
    uint64_t reserved[3];
  } reg;
  if (uring_copy_in(&reg, addr, sizeof(reg))) { return -EFAULT; }
  if (reg.flags || reg.reserved[0] || reg.reserved[1] || reg.reserved[2]) { return -EINVAL; }
  struct uring_group **slot = find(r, reg.id);
  if (op == 23) {
    if (!*slot || !(*slot)->size) { return -2; }
    drop(r, slot);
    return 0;
  }
  if (*slot) { return -17; }
  if (!reg.entries || reg.entries > 32768 || (reg.entries & (reg.entries - 1)) || reg.addr % PAGE_SIZE ||
      reg.addr >= USER_VA_LIMIT || reg.entries * 16u > USER_VA_LIMIT - reg.addr) {
    return -EINVAL;
  }
  unsigned n = page_count(reg.entries);
  if (n > 16384u - r->pinned_pages) { return -ENOMEM; }
  struct uring_group *g = create(slot, reg.id);
  if (!g) { return -ENOMEM; }
  g->size = reg.entries;
  g->pages = table_alloc(n, sizeof(*g->pages));
  if (!g->pages) {
    drop(r, slot);
    return -ENOMEM;
  }
  for (unsigned i = 0; i < n; ++i) {
    uint64_t va = reg.addr + (uint64_t)i * PAGE_SIZE;
    if (!syscall_user_writable(va, 1) || !(g->pages[i] = vmm_pin_writable(syscall_active_as(), va))) {
      drop(r, slot);
      return -EFAULT;
    }
    ++r->pinned_pages;
  }
  return 0;
}

int uring_selected_io(struct uring *r, struct domain *d, struct open_file *target, const struct sqe *e,
                      uint32_t *flags) {
  struct uring_group *g = *find(r, e->buf_index);
  if (!g) { return -105; }
  uint64_t addr;
  uint32_t len;
  uint16_t bid;
  if (g->size) {
    struct descriptor {
      uint64_t addr;
      uint32_t len;
      uint16_t bid, tail;
    };
    struct descriptor *base = pmm_phys_to_virt(g->pages[0]);
    uint16_t ready = __atomic_load_n(&base->tail, __ATOMIC_ACQUIRE) - g->head;
    if (!ready) { return -105; }
    if (ready > g->size) { return -EINVAL; }
    unsigned index = g->head & (g->size - 1);
    struct descriptor *entries = pmm_phys_to_virt(g->pages[index / (PAGE_SIZE / sizeof(*base))]);
    struct descriptor desc = entries[index % (PAGE_SIZE / sizeof(*base))];
    addr = desc.addr;
    len = desc.len;
    bid = desc.bid;
  } else {
    if (!g->first) { return -105; }
    addr = g->first->addr;
    len = g->first->len;
    bid = g->first->bid;
  }
  struct sqe io = *e;
  io.addr = addr;
  io.len = e->len && e->len < len ? e->len : len;
  if (io.len > INT32_MAX) { return -EINVAL; }
  int rc = uring_retry_io(d, target, &io);
  if (rc >= 0) {
    if (g->size) {
      ++g->head;
    } else {
      consume_extent(g, 1);
    }
    *flags = 1u | ((uint32_t)bid << 16);
  }
  return rc;
}
