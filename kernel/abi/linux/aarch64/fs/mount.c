#include "file_abi.h"
#include "../usercopy.h"
#include "fs/vfs.h"
#include "lib/mem.h"

struct fs_info64 {
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
  uint64_t inode_count;
  uint64_t free_inodes;
};

struct mount_info64 {
  char source[32];
  char target[32];
  char fstype[16];
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
};

int64_t sys_spore_fsinfo(uint64_t addr) {
  struct vfs_fs_info raw;
  if (!vfs_fs_info(&raw)) { return -(int64_t)EINVAL; }
  struct fs_info64 info = {
    .block_size = raw.block_size,
    .block_count = raw.block_count,
    .free_blocks = raw.free_blocks,
    .inode_count = raw.inode_count,
    .free_inodes = raw.free_inodes,
  };
  return syscall_user_writable(addr, sizeof(info)) && vmm_copy_to_user(syscall_active_as(), addr, &info, sizeof(info))
           ? 0
           : -(int64_t)EFAULT;
}

int64_t sys_spore_mountinfo(uint64_t addr, uint64_t bytes) {
  size_t max = bytes / sizeof(struct mount_info64);
  if (addr == 0 || max == 0) { return (int64_t)vfs_mount_info(NULL, 0); }
  struct vfs_mount_info raw[8];
  size_t count = vfs_mount_info(raw, max < 8 ? max : 8);
  size_t copy = count < max ? count : max;
  struct mount_info64 out[8];
  for (size_t i = 0; i < copy; ++i) {
    kmemset(&out[i], 0, sizeof(out[i]));
    kmemcpy(out[i].source, raw[i].source, sizeof(out[i].source));
    kmemcpy(out[i].target, raw[i].target, sizeof(out[i].target));
    kmemcpy(out[i].fstype, raw[i].fstype, sizeof(out[i].fstype));
    out[i].block_size = raw[i].block_size;
    out[i].block_count = raw[i].block_count;
    out[i].free_blocks = raw[i].free_blocks;
  }
  return syscall_user_writable(addr, copy * sizeof(out[0])) &&
             vmm_copy_to_user(syscall_active_as(), addr, out, copy * sizeof(out[0]))
           ? (int64_t)count
           : -(int64_t)EFAULT;
}
