#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct spore_ext2_dir_record {
  uint32_t ino;
  uint16_t rec_len;
  uint8_t name_len;
  uint8_t type;
};

bool spore_ext2_parse_dir_record(const uint8_t *data, size_t available, struct spore_ext2_dir_record *out);
