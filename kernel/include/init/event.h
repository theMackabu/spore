#pragma once

#include <stddef.h>
#include <stdint.h>

void app_event(void (*write)(const uint8_t *, size_t), char kind, unsigned value);
