#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct spore_ext2_dir_slot {
  uint32_t offset;
  uint16_t rec_len;
  uint16_t used;
};
struct spore_ext2_dir_match {
  uint32_t offset;
  uint32_t prev_offset;
  uint32_t ino;
  uint16_t rec_len;
};
typedef bool (*ext2_record_visitor)(void *ctx, uint32_t ino, uint8_t type, const char *name, uint8_t len);
struct ext2_fs;
struct ext2_node;
int ext2_visit_dir(struct ext2_fs *fs, const struct ext2_node *dir, uint64_t *cursor, ext2_record_visitor visit,
                   void *ctx);
int spore_ext2_find_dir_slot(const uint8_t *data, size_t len, uint16_t need, struct spore_ext2_dir_slot *out);
int spore_ext2_find_dir_name(const uint8_t *data, size_t len, const uint8_t *name, size_t name_len,
                             struct spore_ext2_dir_match *out);
int spore_ext2_visit_dir_block(const uint8_t *data, size_t len, bool hide_spore, ext2_record_visitor visit, void *ctx,
                               size_t *consumed);
