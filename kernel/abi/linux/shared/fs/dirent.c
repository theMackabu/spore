#include "../usercopy.h"
#include "abi/linux/shared/layout.h"
#include "handlers.h"

#include "fs/vfs.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "proc/fd.h"

enum { EFAULT = 14, EINVAL = 22 };

int64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t len) {
  struct vfs_node dir;
  uint64_t cursor;
  int rc = cell_fd_get_dir_node((int)fd, &dir, &cursor);
  if (rc < 0) { return rc; }

  uint8_t batch[1024];
  uint64_t written = 0;
  struct user_address_space *as = syscall_active_as();
  do {
    uint64_t dest = buf + written;
    size_t page_left = PAGE_SIZE - dest % PAGE_SIZE;
    size_t capacity = page_left < sizeof(batch) ? page_left : sizeof(batch);
    size_t used = 0;
    for (;;) {
      struct vfs_dirent ent;
      uint64_t next = cursor;
      rc = vfs_next_dirent(&dir, &next, &ent);
      if (rc <= 0) { break; }
      size_t size = sizeof(struct linux_dirent64_header) + ent.name_len + 1;
      uint16_t reclen = (uint16_t)((size + 7) & ~7u);
      if (reclen > len - written - used) {
        rc = -EINVAL;
        break;
      }
      if (used && used + reclen > capacity) { break; }
      struct linux_dirent64_header header = {
        .d_ino = ent.ino, .d_off = (int64_t)next, .d_reclen = reclen, .d_type = ent.type,
      };
      kmemcpy(batch + used, &header, sizeof(header));
      kmemcpy(batch + used + sizeof(header), ent.name, (size_t)ent.name_len + 1);
      kmemset(batch + used + size, 0, reclen - size);
      cursor = next;
      used += reclen;
      if (used >= capacity) { break; }
    }
    if (used == 0) { break; }
    if (dest < buf || !syscall_user_writable(dest, used) || !vmm_copy_to_user(as, dest, batch, used)) {
      rc = -EFAULT;
      break;
    }
    written += used;
    cell_fd_set_dir_offset((int)fd, cursor);
  } while (rc > 0 && written < len);
  return written ? (int64_t)written : rc;
}
