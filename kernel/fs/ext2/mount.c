#include "internal.h"

static uint32_t ext2_now;
struct ext2_stats ext2_stat_counters;

void ext2_set_now(uint32_t epoch_sec) {
  ext2_now = epoch_sec;
}

uint32_t ext2_now_sec(void) {
  return ext2_now;
}

struct ext2_stats ext2_get_stats(void) {
  return ext2_stat_counters;
}

bool ext2_read_super(struct ext2_fs *fs, struct ext2_super *out) {
  return ext2_read_bytes(fs, EXT2_SUPER_OFFSET, out, sizeof(*out));
}

bool ext2_write_super(struct ext2_fs *fs, const struct ext2_super *sb) {
  return fs->write != NULL && ext2_write_bytes(fs, EXT2_SUPER_OFFSET, sb, sizeof(*sb));
}

bool ext2_read_group_desc(struct ext2_fs *fs, uint32_t group, struct ext2_group_desc *out) {
  if (group >= fs->group_count) { return false; }
  uint64_t table = fs->block_size == 1024 ? 2ull * fs->block_size : fs->block_size;
  return ext2_read_bytes(fs, table + (uint64_t)group * sizeof(*out), out, sizeof(*out));
}

bool ext2_write_group_desc(struct ext2_fs *fs, uint32_t group, const struct ext2_group_desc *in) {
  if (group >= fs->group_count || fs->write == NULL) { return false; }
  uint64_t table = fs->block_size == 1024 ? 2ull * fs->block_size : fs->block_size;
  return ext2_write_bytes(fs, table + (uint64_t)group * sizeof(*in), in, sizeof(*in));
}

static bool ext2plus_header_valid(struct ext2_fs *fs, const struct ext2plus_header *hdr) {
  return hdr->magic == EXT2PLUS_MAGIC && hdr->version == EXT2PLUS_VERSION && hdr->header_size == sizeof(*hdr) &&
         hdr->inode_size == fs->inode_size && hdr->base_ino > fs->inode_count &&
         hdr->base_ino == fs->ext2plus_base_ino && hdr->chunk_size != 0 && hdr->next_index <= hdr->capacity &&
         (hdr->free_head == EXT2PLUS_FREE_NONE || hdr->free_head < hdr->capacity) && hdr->free_count <= hdr->capacity &&
         hdr->static_inodes_used <= EXT2PLUS_STATIC_INODES_BEFORE_DYNAMIC;
}

static void ext2plus_cache_header(struct ext2_fs *fs, const struct ext2plus_header *hdr) {
  fs->ext2plus_capacity = hdr->capacity;
  fs->ext2plus_next_index = hdr->next_index;
  fs->ext2plus_free_head = hdr->free_head;
  fs->ext2plus_free_count = hdr->free_count;
  fs->ext2plus_chunk_size = hdr->chunk_size;
  fs->ext2plus_static_inodes_used = hdr->static_inodes_used;
}

bool ext2plus_load_header(struct ext2_fs *fs, struct ext2plus_header *hdr) {
  *hdr = (struct ext2plus_header){
    .magic = EXT2PLUS_MAGIC,
    .version = EXT2PLUS_VERSION,
    .header_size = sizeof(*hdr),
    .inode_size = fs->inode_size,
    .base_ino = fs->ext2plus_base_ino,
    .capacity = fs->ext2plus_capacity,
    .next_index = fs->ext2plus_next_index,
    .free_head = fs->ext2plus_free_head,
    .free_count = fs->ext2plus_free_count,
    .chunk_size = fs->ext2plus_chunk_size,
    .static_inodes_used = fs->ext2plus_static_inodes_used,
  };
  return ext2plus_header_valid(fs, hdr);
}

bool ext2plus_write_header(struct ext2_fs *fs, const struct ext2plus_header *hdr) {
  if (!ext2plus_header_valid(fs, hdr) || !ext2plus_write_store(fs, 0, hdr, sizeof(*hdr))) { return false; }
  ext2plus_cache_header(fs, hdr);
  return true;
}

static bool ext2plus_mount_init(struct ext2_fs *fs) {
  struct ext2_node store;
  fs->ext2plus = false;
  fs->ext2plus_store_ino = 0;
  fs->ext2plus_base_ino = EXT2PLUS_BASE_INO;
  if (!ext2_lookup(fs, "/.spore-inodes", &store) || !ext2_is_regular(&store)) { return false; }
  fs->ext2plus_store_ino = store.ino;
  fs->ext2plus_store = store;
  fs->ext2plus_store_valid = true;

  struct ext2plus_header hdr;
  uint32_t got = 0;
  bool valid = store.size >= sizeof(hdr) && ext2_read_file(fs, &store, 0, &hdr, sizeof(hdr), &got) &&
               got == sizeof(hdr) && ext2plus_header_valid(fs, &hdr);
  if (!valid && fs->write != NULL) {
    kmemset(&hdr, 0, sizeof(hdr));
    hdr.magic = EXT2PLUS_MAGIC;
    hdr.version = EXT2PLUS_VERSION;
    hdr.header_size = sizeof(hdr);
    hdr.inode_size = fs->inode_size;
    hdr.base_ino = fs->ext2plus_base_ino;
    hdr.free_head = EXT2PLUS_FREE_NONE;
    hdr.chunk_size = EXT2PLUS_CHUNK_INODES;
    valid = ext2plus_write_header(fs, &hdr);
  }
  if (!valid) { return false; }
  ext2plus_cache_header(fs, &hdr);
  fs->ext2plus = true;
  ext2_dir_cache_remove_name(fs, EXT2_ROOT_INO, ".spore-inodes");
  return true;
}

bool ext2_mount_rw(struct ext2_fs *fs, ext2_read_fn read, ext2_write_fn write, void *ctx) {
  if (fs == NULL || read == NULL) { return false; }
  struct ext2_super sb;
  if (!read(ctx, EXT2_SUPER_OFFSET, &sb, sizeof(sb))) { return false; }
  if (sb.magic != EXT2_SUPER_MAGIC) { return false; }
  if (sb.log_block_size > 2) { return false; }
  uint32_t block_size = 1024u << sb.log_block_size;
  uint32_t inode_size = sb.inode_size ? sb.inode_size : 128u;
  if (inode_size < 128 || inode_size > block_size || (inode_size & (inode_size - 1)) != 0 ||
      sb.first_data_block != (block_size == 1024 ? 1u : 0u) || sb.blocks_count <= sb.first_data_block ||
      sb.blocks_per_group == 0 || sb.blocks_per_group > block_size * 8 ||
      sb.inodes_per_group == 0 || sb.inodes_per_group > block_size * 8) { return false; }
  uint32_t first_ino = sb.rev_level == 0 ? 11u : sb.first_ino;
  if (first_ino < 11 || first_ino - 1 > sb.inodes_count) { return false; }
  uint32_t groups = 1 + (sb.blocks_count - sb.first_data_block - 1) / sb.blocks_per_group;
  if (sb.inodes_count < EXT2_ROOT_INO || sb.inodes_count > (uint64_t)groups * sb.inodes_per_group) { return false; }
  uint32_t supported_incompat = EXT2_INCOMPAT_FILETYPE | EXT2_INCOMPAT_SPORE_EXT2PLUS;
  if ((sb.feature_incompat & ~supported_incompat) != 0) { return false; }

  if (!ext2_drop_cache(fs)) { return false; }
  *fs = (struct ext2_fs){
    .read = read, .write = write, .ctx = ctx,
    .block_size = block_size, .inode_size = inode_size, .group_count = groups,
    .inodes_per_group = sb.inodes_per_group, .blocks_per_group = sb.blocks_per_group,
    .first_data_block = sb.first_data_block, .next_alloc_block = sb.first_data_block,
    .inode_count = sb.inodes_count, .block_count = sb.blocks_count, .first_ino = first_ino, .next_alloc_inode = first_ino, .ext2plus_free_head = EXT2PLUS_FREE_NONE,
  };
  if ((sb.feature_incompat & EXT2_INCOMPAT_SPORE_EXT2PLUS) != 0 && !ext2plus_mount_init(fs)) { return false; }
  return true;
}

bool ext2_mount(struct ext2_fs *fs, ext2_read_fn read, void *ctx) {
  return ext2_mount_rw(fs, read, NULL, ctx);
}

bool ext2_info(struct ext2_fs *fs, struct ext2_info *out) {
  struct ext2_super sb;
  if (fs == NULL || out == NULL || !ext2_read_super(fs, &sb)) { return false; }
  uint32_t inode_count = sb.inodes_count;
  uint32_t free_inodes = sb.free_inodes_count;
  if (fs->ext2plus) {
    struct ext2plus_header hdr;
    if (ext2plus_load_header(fs, &hdr)) {
      uint32_t classic_used = sb.inodes_count - sb.free_inodes_count;
      uint32_t static_left = hdr.static_inodes_used < EXT2PLUS_STATIC_INODES_BEFORE_DYNAMIC
                               ? EXT2PLUS_STATIC_INODES_BEFORE_DYNAMIC - hdr.static_inodes_used
                               : 0;
      if (static_left > sb.free_inodes_count) { static_left = sb.free_inodes_count; }
      inode_count = classic_used + static_left + hdr.capacity;
      free_inodes = static_left + (hdr.capacity - hdr.next_index) + hdr.free_count;
    }
  }
  *out = (struct ext2_info){
    .block_size = fs->block_size,
    .block_count = sb.blocks_count,
    .free_blocks = sb.free_blocks_count,
    .inode_count = inode_count,
    .free_inodes = free_inodes,
  };
  return true;
}
