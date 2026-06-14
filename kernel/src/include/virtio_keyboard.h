#pragma once

#include <stdbool.h>
#include <stdint.h>

bool virtio_keyboard_init(uint64_t hhdm_offset);
bool virtio_keyboard_poll(void);
