#pragma once

#include "boot_info.h"

#include <stdbool.h>
#include <stdint.h>

bool virtio_gpu_init(uint64_t hhdm_offset, struct spore_boot_info *fb_boot);
void virtio_gpu_flush(void);
void virtio_gpu_poll(void);
