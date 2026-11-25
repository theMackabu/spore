#include "proc/fd/record_lock.h"
#include "abi/linux/shared/open.h"
#include "abi/linux/shared/usercopy.h"
#include "handlers.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/pid_namespace.h"

struct linux_flock {
  int16_t type, whence;
  int64_t start, len;
  int32_t pid;
};

int64_t sys_record_lock(uint64_t fd, uint64_t arg, bool query) {
  struct open_file *file = cell_fd_file(fd);
  if (!file || (file->flags & LINUX_O_PATH)) { return -9; }
  if (file->type != OPEN_VFS) { return -9; }
  struct linux_flock lock;
  struct user_address_space *as = syscall_active_as();
  if (!syscall_user_readable(arg, sizeof(lock)) || !vmm_copy_from_user(as, &lock, arg, sizeof(lock))) { return -14; }
  if (lock.type < RECORD_READ || lock.type > (query ? RECORD_WRITE : RECORD_UNLOCK)) { return -22; }
  if (!query && ((lock.type == RECORD_READ && !linux_open_readable(file->flags)) ||
                 (lock.type == RECORD_WRITE && !linux_open_writable(file->flags)))) {
    return -9;
  }
  uint64_t base = 0;
  if (lock.whence == 1) {
    base = file->offset;
  } else if (lock.whence == 2) {
    struct vfs_node node;
    if (!vfs_refresh(&file->node, &node)) { return -5; }
    base = node.size;
  } else if (lock.whence != 0) {
    return -22;
  }
  int64_t start;
  if (base > INT64_MAX || __builtin_add_overflow((int64_t)base, lock.start, &start)) { return -75; }
  if (start < 0) { return -22; }
  uint64_t end = (uint64_t)INT64_MAX + 1;
  if (lock.len > 0) {
    if ((uint64_t)lock.len > end - (uint64_t)start) { return -75; }
    end = (uint64_t)start + lock.len;
  } else if (lock.len < 0) {
    end = start;
    start += lock.len;
    if (start < 0) { return -22; }
  }
  struct record_lock_range range = {.type = lock.type, .start = start, .end = end, .owner = cell_current_pid()};
  int rc = cell_record_lock(file, &range, query);
  if (rc || !query) { return rc; }
  lock.type = range.type;
  if (range.type != RECORD_UNLOCK) {
    lock.whence = 0;
    lock.start = range.start;
    lock.pid = pidns_self_view(range.owner);
    lock.len = range.end > (uint64_t)INT64_MAX ? 0 : (int64_t)(range.end - range.start);
  }
  return syscall_user_writable(arg, sizeof(lock)) && vmm_copy_to_user(as, arg, &lock, sizeof(lock)) ? 0 : -14;
}
