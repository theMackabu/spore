#include "abi/linux/shared/open.h"
#include "abi/linux/shared/usercopy.h"
#include "internal.h"
#include "proc/domain.h"

static uint32_t round_entries(uint32_t n) {
  uint32_t size = 1;
  while (size < n) {
    size <<= 1;
  }
  return size;
}

int64_t sys_io_uring_setup(uint32_t entries, uint64_t addr) {
  struct params p;
  struct user_address_space *as = syscall_active_as();
  if (!syscall_user_readable(addr, sizeof(p)) || !syscall_user_writable(addr, sizeof(p)) ||
      !vmm_copy_from_user(as, &p, addr, sizeof(p))) {
    return -EFAULT;
  }
  if (!entries || p.reserved[0] || p.reserved[1] || p.reserved[2] ||
      (p.flags & ~(SETUP_CQSIZE | SETUP_CLAMP | SETUP_NO_SQARRAY | SETUP_R_DISABLED))) {
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
  file->ring = (struct uring){
    .entries = sq, .cq_entries = cq, .sq_array_offset = array, .disabled = (p.flags & SETUP_R_DISABLED) != 0};
  int fd = cell_install_fd(cell_current_domain_internal(), file, true);
  if (fd >= 0 && !vmm_copy_to_user(as, addr, &p, sizeof(p))) {
    cell_fd_close(fd);
    return -EFAULT;
  }
  return fd;
}

struct open_file *uring_file(int fd) {
  struct open_file *file = cell_fd_file(fd);
  return file && file->type == OPEN_URING ? file : NULL;
}
