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
struct spore_ext2_dir_slot { uint32_t offset; uint16_t rec_len; uint16_t used; };
struct spore_ext2_dir_match { uint32_t offset; uint32_t prev_offset; uint32_t ino; uint16_t rec_len; };
bool spore_ext2_find_dir_slot(const uint8_t *data, size_t len, uint16_t need, struct spore_ext2_dir_slot *out);
bool spore_ext2_find_dir_name(const uint8_t *data, size_t len, const uint8_t *name, size_t name_len, struct spore_ext2_dir_match *out);
