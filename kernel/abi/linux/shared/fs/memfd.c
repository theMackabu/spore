#include "file_abi.h"
#include "abi/linux/shared/open.h"
#include "abi/linux/shared/usercopy.h"
#include "fs/vfs.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/fd.h"

enum { MFD_CLOEXEC = 1, MFD_ALLOW_SEALING = 2 };

int64_t sys_memfd_create(uint64_t name_addr, uint64_t flags) {
  if (flags & ~(uint64_t)(MFD_CLOEXEC | MFD_ALLOW_SEALING)) { return -EINVAL; }
  constexpr size_t NAME_CAP = 250;
  char display[FD_PATH_MAX] = "/memfd:";
  size_t prefix = sizeof("/memfd:") - 1;
  int64_t len = syscall_read_user_string(name_addr, display + prefix, NAME_CAP);
  if (len < 0) { return len == -E2BIG ? -EINVAL : len; }
  static_assert(sizeof("/memfd:") - 1 + NAME_CAP - 1 + sizeof(" (deleted)") <= FD_PATH_MAX);
  kmemcpy(display + prefix + len - 1, " (deleted)", sizeof(" (deleted)"));
  struct vfs_node dir, node;
  if (!vfs_lookup("/run", &dir)) { return -ENOENT; }
  int rc = vfs_tmpfile(&dir, 0777, cell_current_euid(), cell_current_egid(), false, &node);
  if (rc) { return rc; }
  uint32_t open_flags = LINUX_O_RDWR | ((flags & MFD_CLOEXEC) ? LINUX_O_CLOEXEC : 0);
  int fd = vfs_set_sealable(&node, flags & MFD_ALLOW_SEALING) ? cell_fd_open_node(&node, open_flags, display, NULL, NULL) : -EINVAL;
  vfs_release_node(&node);
  return fd;
}
