#pragma once

#include <stdbool.h>
#include <stdint.h>

bool virtio_blk_init(uint64_t hhdm_offset);
bool virtio_blk_read(uint64_t offset, void *dst, uint32_t len);
bool virtio_blk_write(uint64_t offset, const void *src, uint32_t len);
bool virtio_blk_read_boot(uint64_t offset, void *dst, uint32_t len);
