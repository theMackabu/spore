#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool virtio_console_init(uint64_t hhdm_offset);
bool virtio_console_ready(void);
void virtio_console_write(const uint8_t *data, size_t len);
