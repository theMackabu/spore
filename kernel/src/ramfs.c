#include "ramfs.h"

#include "mem.h"

#if __STDC_HOSTED__
#include <stdlib.h>
#else
#include "mm/pmm.h"
#endif

struct ramfs_backing_page {
  bool used;
  int next;
  int owner;
  uint32_t file_page;
  uint64_t pa;
  uint8_t *data;
};

static struct ramfs_backing_page backing_pages[RAMFS_MAX_BACKING_PAGES];
static uint64_t ramfs_now_sec;

void ramfs_set_now(uint64_t epoch_sec) {
  ramfs_now_sec = epoch_sec;
}

static bool streq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) { return false; }
  }
  return *a == '\0' && *b == '\0';
}

static void copy_name(char *dst, const char *src) {
  size_t i = 0;
  for (; i < RAMFS_NAME_MAX && src[i] != '\0'; ++i) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

static const char *last_component(const char *path) {
  const char *name = path;
  for (const char *p = path; *p != '\0'; ++p) {
    if (*p == '/' && p[1] != '\0') { name = p + 1; }
  }
  return name;
}

static int alloc_node(struct ramfs *fs) {
  for (int i = 0; i < RAMFS_MAX_NODES; ++i) {
    if (!fs->nodes[i].used) {
      kmemset(&fs->nodes[i], 0, sizeof(fs->nodes[i]));
      fs->nodes[i].used = true;
      fs->nodes[i].parent = -1;
      fs->nodes[i].first_page = -1;
      fs->nodes[i].ino = fs->next_ino++;
      fs->nodes[i].atime = ramfs_now_sec;
      fs->nodes[i].ctime = ramfs_now_sec;
      fs->nodes[i].mtime = ramfs_now_sec;
      return i;
    }
  }
  return -1;
}

static void free_backing_data(const struct ramfs_backing_page *page) {
  if (page->data == NULL) { return; }
#if __STDC_HOSTED__
  free(page->data);
#else
  if (page->pa != 0) { pmm_free_page(page->pa); }
#endif
}

static bool alloc_backing_data(struct ramfs *fs, struct ramfs_backing_page *page) {
#if __STDC_HOSTED__
  page->data = malloc(RAMFS_PAGE_SIZE);
  if (page->data == NULL) { return false; }
  kmemset(page->data, 0, RAMFS_PAGE_SIZE);
  (void)fs;
  page->pa = 0;
  return true;
#else
  uint64_t pa = pmm_alloc_zero_page();
  if (pa == 0) { return false; }
  page->pa = pa;
  page->data = (uint8_t *)(uintptr_t)(fs->hhdm_offset + pa);
  return true;
#endif
}

static void reset_backing_pages(void) {
  for (size_t i = 0; i < RAMFS_MAX_BACKING_PAGES; ++i) {
    if (backing_pages[i].used) { free_backing_data(&backing_pages[i]); }
  }
  kmemset(backing_pages, 0, sizeof(backing_pages));
  for (size_t i = 0; i < RAMFS_MAX_BACKING_PAGES; ++i) {
    backing_pages[i].next = -1;
    backing_pages[i].owner = -1;
  }
}

uint64_t ramfs_backing_used_pages(void) {
  uint64_t count = 0;
  for (size_t i = 0; i < RAMFS_MAX_BACKING_PAGES; ++i) {
    if (backing_pages[i].used) { ++count; }
  }
  return count;
}

static int find_backing_page(const struct ramfs *fs, int node, uint32_t file_page) {
  for (int slot = fs->nodes[node].first_page; slot >= 0; slot = backing_pages[slot].next) {
    if (backing_pages[slot].file_page == file_page) { return slot; }
  }
  return -1;
}

static int get_backing_page(struct ramfs *fs, int node, uint32_t file_page, bool create) {
  int found = find_backing_page(fs, node, file_page);
  if (found >= 0 || !create) { return found; }
  for (int i = 0; i < RAMFS_MAX_BACKING_PAGES; ++i) {
    if (backing_pages[i].used) { continue; }
    backing_pages[i] = (struct ramfs_backing_page){
      .used = true,
      .next = fs->nodes[node].first_page,
      .owner = node,
      .file_page = file_page,
    };
    if (!alloc_backing_data(fs, &backing_pages[i])) {
      backing_pages[i] = (struct ramfs_backing_page){.next = -1, .owner = -1};
      return -1;
    }
    fs->nodes[node].first_page = i;
    return i;
  }
  return -1;
}

static void free_node_pages(struct ramfs *fs, int node) {
  int slot = fs->nodes[node].first_page;
  fs->nodes[node].first_page = -1;
  while (slot >= 0) {
    int next = backing_pages[slot].next;
    free_backing_data(&backing_pages[slot]);
    backing_pages[slot] = (struct ramfs_backing_page){.next = -1, .owner = -1};
    slot = next;
  }
}

static int find_child(const struct ramfs *fs, int parent, const char *name) {
  for (int i = 0; i < RAMFS_MAX_NODES; ++i) {
    if (fs->nodes[i].used && fs->nodes[i].parent == parent && streq(fs->nodes[i].name, name)) { return i; }
  }
  return -1;
}

static int lookup_index(const struct ramfs *fs, const char *path) {
  if (path == NULL || path[0] != '/') { return -1; }
  if (streq(path, "/")) { return 0; }
  int current = 0;
  const char *p = path + 1;
  char component[RAMFS_NAME_MAX + 1];
  while (*p != '\0') {
    size_t len = 0;
    while (p[len] != '\0' && p[len] != '/') {
      if (len >= RAMFS_NAME_MAX) { return -1; }
      component[len] = p[len];
      ++len;
    }
    component[len] = '\0';
    current = find_child(fs, current, component);
    if (current < 0) { return -1; }
    p += len;
    if (*p == '/') { ++p; }
  }
  return current;
}

static bool split_parent(const struct ramfs *fs, const char *path, int *parent, const char **name) {
  if (path == NULL || path[0] != '/' || streq(path, "/")) { return false; }
  const char *last = last_component(path);
  if (*last == '\0') { return false; }
  char parent_path[128];
  size_t parent_len = (size_t)(last - path);
  if (parent_len == 0) { parent_len = 1; }
  if (parent_len >= sizeof(parent_path)) { return false; }
  for (size_t i = 0; i < parent_len; ++i) {
    parent_path[i] = path[i];
  }
  if (parent_len > 1 && parent_path[parent_len - 1] == '/') { --parent_len; }
  parent_path[parent_len] = '\0';
  int p = lookup_index(fs, parent_path);
  if (p < 0 || !fs->nodes[p].is_dir) { return false; }
  *parent = p;
  *name = last;
  return true;
}

static int add_node(struct ramfs *fs, int parent, const char *name, bool is_dir, bool writable) {
  if (parent < 0 || !fs->nodes[parent].is_dir || find_child(fs, parent, name) >= 0) { return -1; }
  int index = alloc_node(fs);
  if (index < 0) { return -1; }
  fs->nodes[index].parent = parent;
  fs->nodes[index].is_dir = is_dir;
  fs->nodes[index].writable = writable;
  fs->nodes[index].mount = fs->nodes[parent].mount;
  fs->nodes[index].mode = is_dir ? 0777u : 0666u;
  fs->nodes[index].uid = 0;
  fs->nodes[index].gid = 0;
  fs->nodes[parent].mtime = ramfs_now_sec;
  fs->nodes[parent].ctime = ramfs_now_sec;
  copy_name(fs->nodes[index].name, name);
  return index;
}

static bool device_is_block(enum ramfs_device device) {
  return device == RAMFS_DEV_BLK_ROOT || device == RAMFS_DEV_BLK_BOOT;
}

static bool device_is_readonly_text(enum ramfs_device device) {
  return device == RAMFS_DEV_FS_ROOT || device == RAMFS_DEV_FS_BOOT || device == RAMFS_DEV_FS_RAM0 ||
         device == RAMFS_DEV_FS_TMP || device == RAMFS_DEV_PROCINFO || device == RAMFS_DEV_MEMINFO ||
         device == RAMFS_DEV_CPUINFO || device == RAMFS_DEV_UPTIME || device == RAMFS_DEV_MOUNTS ||
         device == RAMFS_DEV_STAT || device == RAMFS_DEV_KMSG || device == RAMFS_DEV_FILESYSTEMS ||
         device == RAMFS_DEV_PARTITIONS || device == RAMFS_DEV_DEVICES || device == RAMFS_DEV_FSSTATS ||
         device == RAMFS_DEV_PROC_PID_STAT || device == RAMFS_DEV_PROC_PID_STATUS ||
         device == RAMFS_DEV_PROC_PID_CMDLINE || device == RAMFS_DEV_PROC_PID_STATM ||
         device == RAMFS_DEV_PROC_PID_COMM || device == RAMFS_DEV_PROC_PID_MOUNTS || device == RAMFS_DEV_PROC_PID_CWD ||
         device == RAMFS_DEV_PROC_PID_EXE;
}

static void set_mount(struct ramfs *fs, int index, enum ramfs_mount mount) {
  if (index >= 0) { fs->nodes[index].mount = mount; }
}

static bool add_ro_file(struct ramfs *fs, const char *path, const void *data, uint64_t size) {
  if (path == NULL || path[0] != '/' || path[1] == '\0') { return false; }
  int parent = 0;
  const char *p = path + 1;
  const char *name = p;
  while (*p != '\0') {
    const char *start = p;
    while (*p != '\0' && *p != '/') {
      ++p;
    }
    size_t len = (size_t)(p - start);
    if (len == 0 || len > RAMFS_NAME_MAX) { return false; }
    while (*p == '/') {
      ++p;
    }
    if (*p == '\0') {
      name = start;
      break;
    }
    char component[RAMFS_NAME_MAX + 1];
    kmemcpy(component, start, len);
    component[len] = '\0';
    int next = find_child(fs, parent, component);
    if (next < 0) {
      next = add_node(fs, parent, component, true, true);
      if (next < 0) { return false; }
    }
    if (!fs->nodes[next].is_dir) { return false; }
    parent = next;
  }
  int index = add_node(fs, parent, name, false, false);
  if (index < 0) { return false; }
  fs->nodes[index].ro_data = data;
  fs->nodes[index].size = size;
  fs->nodes[index].mode = 0444u;
  return true;
}

static bool add_device(struct ramfs *fs, const char *path, enum ramfs_device device) {
  int parent;
  const char *name;
  if (!split_parent(fs, path, &parent, &name)) { return false; }
  int index = add_node(fs, parent, name, false, false);
  if (index < 0) { return false; }
  fs->nodes[index].device = device;
  fs->nodes[index].mode = device_is_block(device) ? 0660u : (device_is_readonly_text(device) ? 0444u : 0666u);
  return true;
}

void ramfs_init(struct ramfs *fs, const struct spore_boot_module *modules, uint32_t module_count,
                uint64_t hhdm_offset) {
  kmemset(fs, 0, sizeof(*fs));
  reset_backing_pages();
  fs->hhdm_offset = hhdm_offset;
  fs->next_ino = 1;

  int root = alloc_node(fs);
  fs->nodes[root].is_dir = true;
  fs->nodes[root].parent = root;
  fs->nodes[root].mount = RAMFS_MOUNT_RAM0;
  copy_name(fs->nodes[root].name, "");

  int dev = add_node(fs, root, "dev", true, true);
  int proc = add_node(fs, root, "proc", true, true);
  int tmp = add_node(fs, root, "tmp", true, true);
  int run = add_node(fs, root, "run", true, true);

  set_mount(fs, dev, RAMFS_MOUNT_DEV);
  set_mount(fs, proc, RAMFS_MOUNT_PROC);
  set_mount(fs, tmp, RAMFS_MOUNT_TMP);
  set_mount(fs, run, RAMFS_MOUNT_RUN);

  (void)add_node(fs, dev, "fs", true, true);
  (void)add_node(fs, dev, "blk", true, true);
  (void)add_node(fs, proc, "net", true, true);

  (void)add_device(fs, "/dev/null", RAMFS_DEV_NULL);
  (void)add_device(fs, "/dev/zero", RAMFS_DEV_ZERO);
  (void)add_device(fs, "/dev/full", RAMFS_DEV_FULL);
  (void)add_device(fs, "/dev/random", RAMFS_DEV_RANDOM);
  (void)add_device(fs, "/dev/urandom", RAMFS_DEV_URANDOM);
  (void)add_device(fs, "/dev/console", RAMFS_DEV_CONSOLE);
  (void)add_device(fs, "/dev/tty", RAMFS_DEV_TTY);
  (void)add_device(fs, "/dev/ttys0", RAMFS_DEV_TTY);
  (void)add_device(fs, "/dev/procinfo", RAMFS_DEV_PROCINFO);
  (void)add_device(fs, "/dev/fs/root", RAMFS_DEV_FS_ROOT);
  (void)add_device(fs, "/dev/fs/boot", RAMFS_DEV_FS_BOOT);
  (void)add_device(fs, "/dev/fs/ram0", RAMFS_DEV_FS_RAM0);
  (void)add_device(fs, "/dev/fs/tmp", RAMFS_DEV_FS_TMP);
  (void)add_device(fs, "/dev/blk/root", RAMFS_DEV_BLK_ROOT);
  (void)add_device(fs, "/dev/blk/boot", RAMFS_DEV_BLK_BOOT);
  (void)add_device(fs, "/proc/procinfo", RAMFS_DEV_PROCINFO);
  (void)add_device(fs, "/proc/meminfo", RAMFS_DEV_MEMINFO);
  (void)add_device(fs, "/proc/cpuinfo", RAMFS_DEV_CPUINFO);
  (void)add_device(fs, "/proc/uptime", RAMFS_DEV_UPTIME);
  (void)add_device(fs, "/proc/loadavg", RAMFS_DEV_LOADAVG);
  (void)add_device(fs, "/proc/mounts", RAMFS_DEV_MOUNTS);
  (void)add_device(fs, "/proc/stat", RAMFS_DEV_STAT);
  (void)add_device(fs, "/proc/net/dev", RAMFS_DEV_NET_DEV);
  (void)add_device(fs, "/proc/kmsg", RAMFS_DEV_KMSG);
  (void)add_device(fs, "/proc/filesystems", RAMFS_DEV_FILESYSTEMS);
  (void)add_device(fs, "/proc/partitions", RAMFS_DEV_PARTITIONS);
  (void)add_device(fs, "/proc/devices", RAMFS_DEV_DEVICES);
  (void)add_device(fs, "/proc/fsstats", RAMFS_DEV_FSSTATS);

  if (modules == NULL) { return; }
  for (uint32_t i = 0; i < module_count; ++i) {
    const struct spore_boot_module *file = &modules[i];
    const char *path = file->path;
    if (path != NULL && path[0] == '/') {
      (void)add_ro_file(fs, path, (const void *)(uintptr_t)(hhdm_offset + file->phys_addr), file->size);
    }
  }
}

bool ramfs_lookup(const struct ramfs *fs, const char *path, struct ramfs_file *out) {
  int index = lookup_index(fs, path);
  if (index < 0 || fs->nodes[index].is_dir || fs->nodes[index].device != RAMFS_DEV_NONE) { return false; }
  out->path = path;
  out->data = fs->nodes[index].writable ? NULL : fs->nodes[index].ro_data;
  out->size = fs->nodes[index].size;
  return true;
}

bool ramfs_refresh_node(struct ramfs *fs, int index, struct ramfs_node *out) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used) { return false; }
  struct ramfs_mem_node *node = &fs->nodes[index];
  *out = (struct ramfs_node){
    .fs = fs,
    .index = index,
    .name = index == 0 ? "/" : node->name,
    .data = node->writable ? NULL : node->ro_data,
    .size = node->size,
    .ino = node->ino,
    .is_dir = node->is_dir,
    .mount = node->mount,
    .device = node->device,
    .mode = node->mode,
    .uid = node->uid,
    .gid = node->gid,
    .atime = node->atime,
    .ctime = node->ctime,
    .mtime = node->mtime,
  };
  return true;
}

bool ramfs_lookup_node(const struct ramfs *fs_const, const char *path, struct ramfs_node *out) {
  struct ramfs *fs = (struct ramfs *)fs_const;
  int index = lookup_index(fs, path);
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_dirent(const struct ramfs *fs, int dir_index, size_t index, struct ramfs_dirent *out) {
  if (dir_index < 0 || dir_index >= RAMFS_MAX_NODES || !fs->nodes[dir_index].used || !fs->nodes[dir_index].is_dir) {
    return false;
  }
  size_t seen = 0;
  for (int i = 0; i < RAMFS_MAX_NODES; ++i) {
    if (fs->nodes[i].used && fs->nodes[i].parent == dir_index && i != dir_index) {
      if (seen == index) {
        out->name = fs->nodes[i].name;
        out->ino = fs->nodes[i].ino;
        out->is_dir = fs->nodes[i].is_dir;
        out->is_device = fs->nodes[i].device != RAMFS_DEV_NONE && fs->nodes[i].mount == RAMFS_MOUNT_DEV;
        out->type = (uint8_t)((fs->nodes[i].mode & 0170000u) >> 12);
        return true;
      }
      ++seen;
    }
  }
  return false;
}

bool ramfs_root_dirent(size_t index, struct ramfs_dirent *out) {
  static const struct ramfs_dirent entries[] = {
    {.name = "dev", .ino = 2, .is_dir = true},
    {.name = "proc", .ino = 3, .is_dir = true},
    {.name = "tmp", .ino = 4, .is_dir = true},
    {.name = "run", .ino = 5, .is_dir = true},
  };
  if (index >= sizeof(entries) / sizeof(entries[0])) { return false; }
  *out = entries[index];
  return true;
}

bool ramfs_mkdir(struct ramfs *fs, const char *path) {
  int parent;
  const char *name;
  return split_parent(fs, path, &parent, &name) && add_node(fs, parent, name, true, true) >= 0;
}

bool ramfs_create(struct ramfs *fs, const char *path, struct ramfs_node *out) {
  int existing = lookup_index(fs, path);
  if (existing >= 0) { return ramfs_refresh_node(fs, existing, out); }
  int parent;
  const char *name;
  if (!split_parent(fs, path, &parent, &name)) { return false; }
  int index = add_node(fs, parent, name, false, true);
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_mkfifo(struct ramfs *fs, const char *path, uint16_t mode, struct ramfs_node *out) {
  if (lookup_index(fs, path) >= 0) { return false; }
  int parent;
  const char *name;
  if (!split_parent(fs, path, &parent, &name)) { return false; }
  int index = add_node(fs, parent, name, false, true);
  if (index < 0) { return false; }
  fs->nodes[index].mode = (uint16_t)(0010000u | (mode & 0777u));
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_mksock(struct ramfs *fs, const char *path, uint16_t mode, struct ramfs_node *out) {
  if (lookup_index(fs, path) >= 0) { return false; }
  int parent;
  const char *name;
  if (!split_parent(fs, path, &parent, &name)) { return false; }
  int index = add_node(fs, parent, name, false, true);
  if (index < 0) { return false; }
  fs->nodes[index].mode = (uint16_t)(0140000u | (mode & 0777u));
  return ramfs_refresh_node(fs, index, out);
}

bool ramfs_truncate(struct ramfs *fs, int index, uint64_t size) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || fs->nodes[index].is_dir ||
      fs->nodes[index].device != RAMFS_DEV_NONE || !fs->nodes[index].writable || size > RAMFS_MAX_FILE_SIZE) {
    return false;
  }
  uint64_t keep_pages = (size + RAMFS_PAGE_SIZE - 1) / RAMFS_PAGE_SIZE;
  int *link = &fs->nodes[index].first_page;
  while (*link >= 0) {
    int slot = *link;
    if (backing_pages[slot].file_page >= keep_pages) {
      *link = backing_pages[slot].next;
      free_backing_data(&backing_pages[slot]);
      backing_pages[slot] = (struct ramfs_backing_page){.next = -1, .owner = -1};
    } else {
      link = &backing_pages[slot].next;
    }
  }
  if ((size % RAMFS_PAGE_SIZE) != 0) {
    int slot = find_backing_page(fs, index, (uint32_t)(size / RAMFS_PAGE_SIZE));
    if (slot >= 0) {
      uint64_t tail = size % RAMFS_PAGE_SIZE;
      kmemset(backing_pages[slot].data + tail, 0, (size_t)(RAMFS_PAGE_SIZE - tail));
    }
  }
  fs->nodes[index].size = size;
  fs->nodes[index].mtime = ramfs_now_sec;
  fs->nodes[index].ctime = ramfs_now_sec;
  return true;
}

bool ramfs_chmod_node(struct ramfs *fs, int index, uint16_t mode) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used) { return false; }
  fs->nodes[index].mode = mode & 07777u;
  fs->nodes[index].ctime = ramfs_now_sec;
  return true;
}

bool ramfs_chown_node(struct ramfs *fs, int index, uint32_t uid, uint32_t gid) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used) { return false; }
  fs->nodes[index].uid = uid;
  fs->nodes[index].gid = gid;
  fs->nodes[index].ctime = ramfs_now_sec;
  return true;
}

bool ramfs_unlink(struct ramfs *fs, const char *path) {
  int index = lookup_index(fs, path);
  if (index <= 0 || !fs->nodes[index].used) { return false; }
  if (fs->nodes[index].is_dir) {
    for (int i = 0; i < RAMFS_MAX_NODES; ++i) {
      if (fs->nodes[i].used && fs->nodes[i].parent == index) { return false; }
    }
  }
  free_node_pages(fs, index);
  fs->nodes[index].used = false;
  return true;
}

bool ramfs_link(struct ramfs *fs, const char *old_path, const char *new_path) {
  int src = lookup_index(fs, old_path);
  if (src <= 0 || fs->nodes[src].is_dir || fs->nodes[src].device != RAMFS_DEV_NONE) { return false; }
  int parent;
  const char *name;
  if (!split_parent(fs, new_path, &parent, &name) || find_child(fs, parent, name) >= 0) { return false; }
  int dst = add_node(fs, parent, name, false, fs->nodes[src].writable);
  if (dst < 0) { return false; }
  fs->nodes[dst].size = fs->nodes[src].size;
  fs->nodes[dst].mode = fs->nodes[src].mode;
  fs->nodes[dst].atime = fs->nodes[src].atime;
  fs->nodes[dst].mtime = fs->nodes[src].mtime;
  fs->nodes[dst].ctime = ramfs_now_sec;
  if (fs->nodes[src].writable) {
    for (int slot = fs->nodes[src].first_page; slot >= 0; slot = backing_pages[slot].next) {
      int dst_slot = get_backing_page(fs, dst, backing_pages[slot].file_page, true);
      if (dst_slot < 0) {
        free_node_pages(fs, dst);
        fs->nodes[dst].used = false;
        return false;
      }
      kmemcpy(backing_pages[dst_slot].data, backing_pages[slot].data, RAMFS_PAGE_SIZE);
    }
  } else {
    fs->nodes[dst].ro_data = fs->nodes[src].ro_data;
  }
  return true;
}

bool ramfs_rename(struct ramfs *fs, const char *old_path, const char *new_path) {
  int index = lookup_index(fs, old_path);
  int parent;
  const char *name;
  if (index <= 0 || !split_parent(fs, new_path, &parent, &name)) { return false; }
  int existing = lookup_index(fs, new_path);
  if (existing > 0) {
    free_node_pages(fs, existing);
    fs->nodes[existing].used = false;
  }
  fs->nodes[index].parent = parent;
  copy_name(fs->nodes[index].name, name);
  fs->nodes[index].ctime = ramfs_now_sec;
  return true;
}

uint64_t ramfs_read(struct ramfs *fs, int index, uint64_t off, void *dst, uint64_t len) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || fs->nodes[index].is_dir ||
      fs->nodes[index].device != RAMFS_DEV_NONE || off >= fs->nodes[index].size) {
    return 0;
  }
  uint64_t n = fs->nodes[index].size - off;
  if (n > len) { n = len; }
  if (!fs->nodes[index].writable) {
    const uint8_t *src = fs->nodes[index].ro_data;
    kmemcpy(dst, src + off, (size_t)n);
    return n;
  }
  uint8_t *out = dst;
  uint64_t done = 0;
  while (done < n) {
    uint64_t pos = off + done;
    uint32_t page_no = (uint32_t)(pos / RAMFS_PAGE_SIZE);
    uint64_t within = pos % RAMFS_PAGE_SIZE;
    uint64_t chunk = RAMFS_PAGE_SIZE - within;
    if (chunk > n - done) { chunk = n - done; }
    int slot = find_backing_page(fs, index, page_no);
    if (slot >= 0) {
      kmemcpy(out + done, backing_pages[slot].data + within, (size_t)chunk);
    } else {
      kmemset(out + done, 0, (size_t)chunk);
    }
    done += chunk;
  }
  return n;
}

int64_t ramfs_write(struct ramfs *fs, int index, uint64_t off, const void *src, uint64_t len) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || fs->nodes[index].is_dir ||
      fs->nodes[index].device != RAMFS_DEV_NONE || !fs->nodes[index].writable || off > RAMFS_MAX_FILE_SIZE ||
      len > RAMFS_MAX_FILE_SIZE - off) {
    return -1;
  }
  const uint8_t *in = src;
  uint64_t done = 0;
  while (done < len) {
    uint64_t pos = off + done;
    uint32_t page_no = (uint32_t)(pos / RAMFS_PAGE_SIZE);
    uint64_t within = pos % RAMFS_PAGE_SIZE;
    uint64_t chunk = RAMFS_PAGE_SIZE - within;
    if (chunk > len - done) { chunk = len - done; }
    int slot = get_backing_page(fs, index, page_no, true);
    if (slot < 0) { return -1; }
    kmemcpy(backing_pages[slot].data + within, in + done, (size_t)chunk);
    done += chunk;
  }
  if (off + len > fs->nodes[index].size) { fs->nodes[index].size = off + len; }
  fs->nodes[index].mtime = ramfs_now_sec;
  fs->nodes[index].ctime = ramfs_now_sec;
  return (int64_t)len;
}
