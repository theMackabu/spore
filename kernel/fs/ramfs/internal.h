#pragma once

#include "fs/ramfs.h"

struct ramfs_mem_node {
  bool is_dir;
  bool writable;
  enum ramfs_mount mount;
  enum ramfs_device device;
  uint16_t mode;
  uint32_t uid;
  uint32_t gid;
  uint32_t refs;
  uint16_t links_count;
  uint32_t seals;
  uint32_t shared_write_mappings;
  bool sealable;
  bool linkable;
  int link_target;
  int first_page;
  uint32_t allocated_pages;
  int parent;
  int hash_next;
  uint32_t name_hash;
  unsigned children;
  uint8_t name_len;
  char name[RAMFS_NAME_MAX + 1];
  const void *ro_data;
  uint64_t size;
  uint64_t ino;
  uint64_t atime;
  uint64_t ctime;
  uint64_t mtime;
};

struct ramfs_slot {
  struct ramfs_mem_node *node;
  int bucket;
  int free_next;
};

struct ramfs_table {
  struct ramfs *owner;
  struct ramfs_table *next;
  int capacity, next_unused, first_free, live;
  struct ramfs_slot slots[];
};

static inline struct ramfs_mem_node *ramfs_inode_at(const struct ramfs *fs, int index) {
  return fs->table->slots[index].node;
}

static inline struct ramfs_mem_node *ramfs_get_inode(const struct ramfs *fs, int index) {
  return fs && fs->table && index >= 0 && index < fs->table->next_unused ? ramfs_inode_at(fs, index) : NULL;
}

int ramfs_alloc_inode(struct ramfs *fs);
void ramfs_free_inode(struct ramfs *fs, int index);
extern uint64_t ramfs_now_sec;
void ramfs_free_node_pages(struct ramfs_mem_node *node);
int ramfs_add_node(struct ramfs *fs, int parent, const char *name, bool is_dir, bool writable);
int ramfs_find_child(const struct ramfs *fs, int parent, const char *name);
bool ramfs_populate(struct ramfs *fs, int root, const struct spore_boot_module *modules, uint32_t count);
