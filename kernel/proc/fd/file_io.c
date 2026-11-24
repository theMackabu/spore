#include "file_io.h"
#include "drivers/virtio_blk.h"
#include "fs/notify.h"
#include "fs/vfs.h"
#include "lib/mem.h"
#include "lib/random.h"
#include "mm/pmm.h"
#include "mm/table.h"
#include "proc/fd.h"
#include "proc/io.h"
#include "proc/memory.h"

constexpr size_t FILE_IO_CHUNK = 128 * 1024;

struct io_buffer {
  void *data;
  size_t len;
  bool direct;
};

static struct io_buffer staging_buffer(size_t len) {
  if (len > FILE_IO_CHUNK) { len = FILE_IO_CHUNK; }
  static uint8_t *large;
  if (len > PAGE_SIZE) {
    if (!large) { large = table_alloc(1, FILE_IO_CHUNK); }
    if (large) { return (struct io_buffer){large, len, false}; }
    len = PAGE_SIZE;
  }
  alignas(64) static uint8_t small[PAGE_SIZE];
  return (struct io_buffer){small, len, false};
}

static struct io_buffer io_buffer(const struct user_address_space *as, uint64_t va, size_t len, enum vmm_access access) {
  if (len > FILE_IO_CHUNK) { len = FILE_IO_CHUNK; }
  void *direct = vmm_direct_user_buffer(as, va, len, access);
  return direct ? (struct io_buffer){direct, len, true} : staging_buffer(len);
}

int64_t cell_read_regular(struct domain *domain, struct open_file *file, uint64_t off, uint64_t buf, uint64_t len) {
  if (!vfs_refresh(&file->node, &file->node)) { return -5; }
  if (off >= file->node.size || !len) { return 0; }
  if (len > file->node.size - off) { len = file->node.size - off; }
  struct user_address_space *as = cell_domain_as(domain);
  struct io_buffer io = io_buffer(as, buf, len, VMM_ACCESS_WRITE);
  size_t got = vfs_read(&file->node, off, io.data, io.len);
  if (!got) { return 0; }
  if (!io.direct && !vmm_copy_to_user(as, buf, io.data, got)) { return -14; }
  fsnotify_file_event(file, FS_ACCESS);
  return (int64_t)got;
}

int64_t cell_write_regular(struct domain *domain, struct open_file *file, uint64_t off, uint64_t buf, uint64_t len) {
  struct user_address_space *as = cell_domain_as(domain);
  struct io_buffer io = staging_buffer(len);
  if (!vmm_copy_from_user(as, io.data, buf, io.len)) { return -14; }
  int64_t wrote = vfs_write(&file->node, off, io.data, io.len);
  if (wrote > 0) { fsnotify_file_event(file, FS_MODIFY); }
  return wrote == 0 && io.len ? -28 : wrote;
}

int64_t cell_read_generated(const struct user_address_space *as, uint64_t buf, size_t len, bool random) {
  struct io_buffer io = io_buffer(as, buf, len, VMM_ACCESS_WRITE);
  if (random) { random_bytes(io.data, io.len); }
  else { kmemset(io.data, 0, io.len); }
  if (io.direct) { return (int64_t)io.len; }
  bool copied = vmm_copy_to_user(as, buf, io.data, io.len);
  if (random) { kmemset(io.data, 0, io.len); }
  return copied ? (int64_t)io.len : -14;
}

int64_t cell_block_device_io(struct domain *domain, struct open_file *file, uint64_t *offset, uint64_t buf, size_t len, bool write) {
  struct user_address_space *as = cell_domain_as(domain);
  struct io_buffer io = io_buffer(as, buf, len, write ? VMM_ACCESS_READ : VMM_ACCESS_WRITE);
  if (write) {
    if (!io.direct && !vmm_copy_from_user(as, io.data, buf, io.len)) { return -14; }
    if (!virtio_blk_write(*offset, io.data, (uint32_t)io.len)) { return -5; }
  } else {
    bool ok = file->node.device == RAMFS_DEV_BLK_BOOT ? virtio_blk_read_boot(*offset, io.data, (uint32_t)io.len)
                                                       : virtio_blk_read(*offset, io.data, (uint32_t)io.len);
    if (!ok) { return -5; }
    if (!io.direct && !vmm_copy_to_user(as, buf, io.data, io.len)) { return -14; }
  }
  *offset += io.len;
  return (int64_t)io.len;
}
