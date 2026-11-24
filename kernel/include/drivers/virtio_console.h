#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool virtio_console_init(uint64_t hhdm_offset);
bool virtio_console_init_at(uint64_t hhdm_offset, uintptr_t base, unsigned slots, unsigned stride);
bool virtio_console_ready(void);
void virtio_console_write(const uint8_t *data, size_t len);
