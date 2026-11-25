#include "internal.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "tty/pty.h"

#if __STDC_HOSTED__
static uint32_t ramfs_smp_possible_cpu_count(void) {
  return 1;
}
#else
#include "arch/smp.h"
static uint32_t ramfs_smp_possible_cpu_count(void) {
  return smp_possible_cpu_count();
}
#endif

uint64_t ramfs_boot_page(const struct ramfs_node *node, uint64_t off) {
  const struct ramfs_mem_node *inode = ramfs_inode_at(node->fs, node->index);
  if (!inode || !inode->ro_data || inode->writable || off > inode->size || inode->size - off < PAGE_SIZE) { return 0; }
  uintptr_t pa = (uintptr_t)inode->ro_data - node->fs->hhdm_offset + off;
  return pa % PAGE_SIZE ? 0 : pa;
}

bool ramfs_add_boot_file(struct ramfs *fs, const char *path, const void *data, uint64_t size, uint16_t mode) {
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
    int next = ramfs_find_child(fs, parent, component);
    if (next < 0) {
      next = ramfs_add_node(fs, parent, component, true, true);
      if (next < 0) { return false; }
    }
    if (!ramfs_inode_at(fs, next)->is_dir) { return false; }
    parent = next;
  }
  int index = ramfs_add_node(fs, parent, name, false, false);
  if (index < 0) { return false; }
  ramfs_inode_at(fs, index)->ro_data = data;
  ramfs_inode_at(fs, index)->size = size;
  ramfs_inode_at(fs, index)->mode = 0100000u | mode;
  return true;
}

static size_t append_dec(char *dst, size_t cap, size_t len, uint64_t value) {
  char tmp[20];
  size_t n = 0;
  if (value == 0) {
    tmp[n++] = '0';
  } else {
    while (value != 0 && n < sizeof(tmp)) {
      tmp[n++] = (char)('0' + (value % 10));
      value /= 10;
    }
  }
  while (n > 0 && len + 1 < cap) {
    dst[len++] = tmp[--n];
  }
  if (cap != 0) { dst[len] = '\0'; }
  return len;
}

enum {
  CHAR_DEVICE = 0020666,
  BLOCK_DEVICE = 0060660,
  READ_ONLY_FILE = 0100444,
};

struct boot_node {
  const char *name;
  enum ramfs_device device;
  uint16_t mode;
};

static bool add_device(struct ramfs *fs, int parent, const char *name, enum ramfs_device device, uint16_t mode) {
  int i = ramfs_add_node(fs, parent, name, false, false);
  if (i >= 0) {
    ramfs_inode_at(fs, i)->device = device;
    ramfs_inode_at(fs, i)->mode = mode;
  }
  return i >= 0;
}

static int add_mount(struct ramfs *fs, int root, const char *name, enum ramfs_mount mount) {
  int i = ramfs_add_node(fs, root, name, true, true);
  if (i >= 0) { ramfs_inode_at(fs, i)->mount = mount; }
  return i;
}

static bool populate_dev(struct ramfs *fs, int dev) {
  int mounts = ramfs_add_node(fs, dev, "fs", true, true);
  int blk = ramfs_add_node(fs, dev, "blk", true, true);
  int shm = ramfs_add_node(fs, dev, "shm", true, true);
  if (shm < 0) { return false; }
  ramfs_inode_at(fs, shm)->mode = 0041777;
  int pts = ramfs_add_node(fs, dev, "pts", true, true);
  static const struct boot_node devices[] = {
    {"null", RAMFS_DEV_NULL, CHAR_DEVICE}, {"zero", RAMFS_DEV_ZERO, CHAR_DEVICE},
    {"full", RAMFS_DEV_FULL, CHAR_DEVICE}, {"random", RAMFS_DEV_RANDOM, CHAR_DEVICE},
    {"urandom", RAMFS_DEV_URANDOM, CHAR_DEVICE}, {"console", RAMFS_DEV_CONSOLE, CHAR_DEVICE},
    {"tty", RAMFS_DEV_TTY, CHAR_DEVICE}, {"ttys0", RAMFS_DEV_TTY, CHAR_DEVICE},
    {"ptmx", RAMFS_DEV_PTMX, CHAR_DEVICE}, {"procinfo", RAMFS_DEV_PROCINFO, READ_ONLY_FILE},
  };
  for (size_t i = 0; i < sizeof(devices) / sizeof(*devices); ++i) {
    if (!add_device(fs, dev, devices[i].name, devices[i].device, devices[i].mode)) { return false; }
  }
  for (unsigned i = 0; i < CELL_PTY_CAP; ++i) {
    char name[12];
    append_dec(name, sizeof(name), 0, i);
    if (!add_device(fs, pts, name, RAMFS_DEV_PTS, CHAR_DEVICE)) { return false; }
  }
  return add_device(fs, mounts, "root", RAMFS_DEV_FS_ROOT, READ_ONLY_FILE) &&
         add_device(fs, mounts, "boot", RAMFS_DEV_FS_BOOT, READ_ONLY_FILE) &&
         add_device(fs, mounts, "ram0", RAMFS_DEV_FS_RAM0, READ_ONLY_FILE) &&
         add_device(fs, mounts, "tmp", RAMFS_DEV_FS_TMP, READ_ONLY_FILE) &&
         add_device(fs, blk, "root", RAMFS_DEV_BLK_ROOT, BLOCK_DEVICE) &&
         add_device(fs, blk, "boot", RAMFS_DEV_BLK_BOOT, BLOCK_DEVICE);
}

static bool populate_proc(struct ramfs *fs, int proc) {
  static const char overflow_id[] = "65534\n";
  if (!ramfs_add_boot_file(fs, "/proc/sys/kernel/overflowuid", overflow_id, sizeof(overflow_id) - 1, 0444) ||
      !ramfs_add_boot_file(fs, "/proc/sys/kernel/overflowgid", overflow_id, sizeof(overflow_id) - 1, 0444)) { return false; }
  int net = ramfs_add_node(fs, proc, "net", true, true);
  if (!add_device(fs, net, "dev", RAMFS_DEV_NET_DEV, READ_ONLY_FILE)) { return false; }
  static const struct boot_node files[] = {
    {"procinfo", RAMFS_DEV_PROCINFO, READ_ONLY_FILE},
    {"meminfo", RAMFS_DEV_MEMINFO, READ_ONLY_FILE},
    {"cpuinfo", RAMFS_DEV_CPUINFO, READ_ONLY_FILE},
    {"uptime", RAMFS_DEV_UPTIME, READ_ONLY_FILE},
    {"loadavg", RAMFS_DEV_LOADAVG, READ_ONLY_FILE},
    {"mounts", RAMFS_DEV_MOUNTS, READ_ONLY_FILE},
    {"stat", RAMFS_DEV_STAT, READ_ONLY_FILE},
    {"kmsg", RAMFS_DEV_KMSG, READ_ONLY_FILE},
    {"filesystems", RAMFS_DEV_FILESYSTEMS, READ_ONLY_FILE},
    {"partitions", RAMFS_DEV_PARTITIONS, READ_ONLY_FILE},
    {"devices", RAMFS_DEV_DEVICES, READ_ONLY_FILE},
    {"fsstats", RAMFS_DEV_FSSTATS, READ_ONLY_FILE},
  };
  for (size_t i = 0; i < sizeof(files) / sizeof(*files); ++i) {
    if (!add_device(fs, proc, files[i].name, files[i].device, files[i].mode)) { return false; }
  }
  return true;
}

static bool populate_cpu(struct ramfs *fs, int sys) {
  int devices = ramfs_add_node(fs, sys, "devices", true, true);
  int system = ramfs_add_node(fs, devices, "system", true, true);
  int cpu = ramfs_add_node(fs, system, "cpu", true, true);
  if (!add_device(fs, cpu, "possible", RAMFS_DEV_SYS_CPU_POSSIBLE, READ_ONLY_FILE)) { return false; }
  if (!add_device(fs, cpu, "present", RAMFS_DEV_SYS_CPU_PRESENT, READ_ONLY_FILE)) { return false; }
  if (!add_device(fs, cpu, "online", RAMFS_DEV_SYS_CPU_ONLINE, READ_ONLY_FILE)) { return false; }
  uint32_t possible = ramfs_smp_possible_cpu_count();
  if (!possible) { possible = 1; }
  for (uint32_t i = 0; i < possible && i < SPORE_BOOT_CPU_MAX; ++i) {
    char name[16] = "cpu";
    append_dec(name, sizeof(name), 3, i);
    int core = ramfs_add_node(fs, cpu, name, true, true);
    if (!add_device(fs, core, "online", RAMFS_DEV_SYS_CPU_CORE_ONLINE, READ_ONLY_FILE)) { return false; }
  }
  return true;
}

bool ramfs_populate(struct ramfs *fs, int root, const struct spore_boot_module *modules, uint32_t count) {
  int dev = add_mount(fs, root, "dev", RAMFS_MOUNT_DEV);
  int proc = add_mount(fs, root, "proc", RAMFS_MOUNT_PROC);
  int sys = add_mount(fs, root, "sys", RAMFS_MOUNT_SYS);
  int tmp = add_mount(fs, root, "tmp", RAMFS_MOUNT_TMP);
  if (tmp < 0) { return false; }
  ramfs_inode_at(fs, tmp)->mode = 0041777;
  if (add_mount(fs, root, "run", RAMFS_MOUNT_RUN) < 0) { return false; }
  if (!populate_dev(fs, dev) ||
      !populate_proc(fs, proc) || !populate_cpu(fs, sys)) { return false; }
  for (uint32_t i = 0; modules && i < count; ++i) {
    const struct spore_boot_module *m = &modules[i];
    struct ramfs_node existing;
    if (*m->path != '/' || ramfs_lookup_node(fs, m->path, &existing)) { continue; }
    if (!ramfs_add_boot_file(fs, m->path, (const void *)(uintptr_t)(fs->hhdm_offset + m->phys_addr), m->size, 0444)) { return false; }
  }
  return true;
}
