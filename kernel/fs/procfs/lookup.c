#include "fs/procfs.h"
#include "fs/vfs.h"
#include "proc/info.h"

#include "proc/accounting.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/fd.h"

enum { VFS_PROC_FD_MAX = 64, VFS_PROC_FD_PATH_MAX = 256 };

static bool parse_uint_component(const char **p, int *out) {
  int value = 0;
  const char *s = *p;
  if (*s < '0' || *s > '9') { return false; }
  while (*s >= '0' && *s <= '9') {
    value = value * 10 + (*s - '0');
    ++s;
  }
  *p = s;
  *out = value;
  return true;
}

static bool proc_kind_for_name(const char *name, enum ramfs_device *device) {
  if (str_eq(name, "stat")) {
    *device = RAMFS_DEV_PROC_PID_STAT;
  } else if (str_eq(name, "status")) {
    *device = RAMFS_DEV_PROC_PID_STATUS;
  } else if (str_eq(name, "cmdline")) {
    *device = RAMFS_DEV_PROC_PID_CMDLINE;
  } else if (str_eq(name, "statm")) {
    *device = RAMFS_DEV_PROC_PID_STATM;
  } else if (str_eq(name, "comm")) {
    *device = RAMFS_DEV_PROC_PID_COMM;
  } else if (str_eq(name, "mounts")) {
    *device = RAMFS_DEV_PROC_PID_MOUNTS;
  } else if (str_eq(name, "mountinfo")) {
    *device = RAMFS_DEV_PROC_PID_MOUNTINFO;
  } else if (str_eq(name, "cwd")) {
    *device = RAMFS_DEV_PROC_PID_CWD;
  } else if (str_eq(name, "exe")) {
    *device = RAMFS_DEV_PROC_PID_EXE;
  } else {
    return false;
  }
  return true;
}

static bool proc_pid_file_node(int pid, enum ramfs_device device, struct vfs_node *out) {
  uint64_t now = cell_realtime_seconds();
  uint16_t mode = 0100444u;
  uint64_t size = 0;

  if (device == RAMFS_DEV_PROC_PID_CWD || device == RAMFS_DEV_PROC_PID_EXE) {
    struct proc_info info;
    mode = 0120777u;
    if (cell_proc_info_for_pid(pid, &info)) {
      const char *target = device == RAMFS_DEV_PROC_PID_CWD ? info.cwd : info.exec_path;
      size = kstrlen(target[0] == '\0' ? "-" : target);
    }
  }

  *out = (struct vfs_node){
    .backend = VFS_PROC,
    .ino = 110000u + (uint64_t)pid * 16u + (uint64_t)device,
    .is_dir = false,
    .device = device,
    .mode = mode,
    .links_count = 1,
    .uid = cell_proc_uid(pid),
    .gid = cell_proc_gid(pid),
    .dev_id = VFS_DEV_PROC,
    .atime = now,
    .ctime = now,
    .mtime = now,
    .proc.pid = pid,
    .size = size,
  };
  return true;
}

static bool proc_fd_dir_node(int pid, struct vfs_node *out) {
  uint64_t now = cell_realtime_seconds();
  *out = (struct vfs_node){
    .backend = VFS_PROC,
    .ino = 140000u + (uint64_t)pid,
    .is_dir = true,
    .device = RAMFS_DEV_PROC_PID_FD,
    .mode = 0040555u,
    .links_count = 1,
    .uid = cell_proc_uid(pid),
    .gid = cell_proc_gid(pid),
    .dev_id = VFS_DEV_PROC,
    .atime = now,
    .ctime = now,
    .mtime = now,
    .proc = {.pid = pid, .fd = -1},
  };
  return true;
}

static bool proc_fd_link_node(int pid, int fd, struct vfs_node *out) {
  if (pid != cell_current_pid()) { return false; }
  struct vfs_node fd_node;
  if (!cell_fd_stat(fd, &fd_node)) { return false; }
  uint64_t now = cell_realtime_seconds();
  *out = (struct vfs_node){
    .backend = VFS_PROC,
    .ino = 150000u + (uint64_t)pid * 64u + (uint64_t)fd,
    .is_dir = false,
    .device = RAMFS_DEV_PROC_PID_FD,
    .mode = 0120777u,
    .links_count = 1,
    .uid = cell_proc_uid(pid),
    .gid = cell_proc_gid(pid),
    .dev_id = VFS_DEV_PROC,
    .atime = now,
    .ctime = now,
    .mtime = now,
    .proc = {.pid = pid, .fd = fd},
  };
  return true;
}

static void utoa_dec(uint64_t value, char *dst, size_t cap) {
  char tmp[32];
  size_t n = 0;
  if (cap == 0) { return; }
  if (value == 0) { tmp[n++] = '0'; }
  while (value != 0 && n < sizeof(tmp)) {
    tmp[n++] = (char)('0' + (value % 10));
    value /= 10;
  }
  size_t out = 0;
  while (n > 0 && out + 1 < cap) {
    dst[out++] = tmp[--n];
  }
  dst[out] = '\0';
}

static size_t append_text(char *dst, size_t cap, size_t len, const char *text) {
  while (*text != '\0' && len + 1 < cap) {
    dst[len++] = *text++;
  }
  if (cap != 0) { dst[len] = '\0'; }
  return len;
}

static size_t append_dec_text(char *dst, size_t cap, size_t len, uint64_t value) {
  char tmp[32];
  utoa_dec(value, tmp, sizeof(tmp));
  return append_text(dst, cap, len, tmp);
}

static bool fd_link_target(int fd, char *out, size_t cap, size_t *len_out) {
  if (cell_fd_path(fd, out, cap)) {
    if (len_out != NULL) { *len_out = kstrlen(out); }
    return true;
  }
  struct vfs_node node;
  if (!cell_fd_stat(fd, &node)) { return false; }
  uint32_t type = node.mode & 0170000u;
  size_t len = 0;
  if (type == 0010000u) {
    len = append_text(out, cap, len, "pipe:[");
    len = append_dec_text(out, cap, len, node.ino);
    len = append_text(out, cap, len, "]");
  } else if (type == 0140000u) {
    len = append_text(out, cap, len, "socket:[");
    len = append_dec_text(out, cap, len, node.ino);
    len = append_text(out, cap, len, "]");
  } else {
    len = append_text(out, cap, len, "anon_inode:[fd");
    len = append_dec_text(out, cap, len, (uint64_t)fd);
    len = append_text(out, cap, len, "]");
  }
  if (len_out != NULL) { *len_out = len; }
  return cap != 0;
}

static bool proc_pid_link_target(const struct vfs_node *node, char *out, size_t cap, size_t *len_out) {
  struct proc_info info;
  if (!cell_proc_info_for_pid(node->proc.pid, &info)) { return false; }
  const char *target = NULL;
  if (node->device == RAMFS_DEV_PROC_PID_CWD) {
    target = info.cwd;
  } else if (node->device == RAMFS_DEV_PROC_PID_EXE) {
    target = info.exec_path;
  } else {
    return false;
  }
  if (target == NULL || target[0] == '\0') { target = "-"; }
  size_t target_len = kstrlen(target);
  if (len_out != NULL) { *len_out = target_len; }
  size_t copy_len = target_len;
  if (copy_len > cap) { copy_len = cap; }
  if (copy_len != 0) { kmemcpy(out, target, copy_len); }
  return true;
}

bool vfs_proc_lookup(const char *path, struct vfs_node *out) {
  if (!str_eq(path, "/proc") && !starts_with(path, "/proc/")) { return false; }
  const char *p = path + 5;
  if (*p != '/') { return false; }
  ++p;
  int pid = 0;
  if (starts_with(p, "self") && (p[4] == '\0' || p[4] == '/')) {
    p += 4;
    pid = cell_current_pid();
  } else if (!parse_uint_component(&p, &pid)) {
    return false;
  }
  if (!cell_proc_exists(pid)) { return false; }
  if (*p == '\0') {
    uint64_t now = cell_realtime_seconds();
    *out = (struct vfs_node){
      .backend = VFS_PROC,
      .ino = 100000u + (uint64_t)pid,
      .is_dir = true,
      .device = RAMFS_DEV_NONE,
      .mode = 0040555u,
      .links_count = 1,
      .uid = cell_proc_uid(pid),
      .gid = cell_proc_gid(pid),
      .dev_id = VFS_DEV_PROC,
      .atime = now,
      .ctime = now,
      .mtime = now,
      .proc.pid = pid,
    };
    return true;
  }
  if (*p != '/') { return false; }
  ++p;
  if (str_eq(p, "fd")) { return proc_fd_dir_node(pid, out); }
  if (starts_with(p, "fd/")) {
    p += 3;
    int fd = 0;
    if (!parse_uint_component(&p, &fd) || *p != '\0') { return false; }
    return proc_fd_link_node(pid, fd, out);
  }
  if (str_eq(p, "task")) {
    uint64_t now = cell_realtime_seconds();
    *out = (struct vfs_node){
      .backend = VFS_PROC,
      .ino = 120000u + (uint64_t)pid,
      .is_dir = true,
      .device = RAMFS_DEV_NONE,
      .mode = 0040555u,
      .links_count = 1,
      .uid = cell_proc_uid(pid),
      .gid = cell_proc_gid(pid),
      .dev_id = VFS_DEV_PROC,
      .atime = now,
      .ctime = now,
      .mtime = now,
      .proc.pid = -pid,
    };
    return true;
  }
  if (starts_with(p, "task/")) {
    p += 5;
    int tid = 0;
    if (!parse_uint_component(&p, &tid) || tid != pid || !cell_proc_exists(tid)) { return false; }
    if (*p == '\0') {
      uint64_t now = cell_realtime_seconds();
      *out = (struct vfs_node){
        .backend = VFS_PROC,
        .ino = 130000u + (uint64_t)pid,
        .is_dir = true,
        .device = RAMFS_DEV_NONE,
        .mode = 0040555u,
        .links_count = 1,
        .uid = cell_proc_uid(pid),
        .gid = cell_proc_gid(pid),
        .dev_id = VFS_DEV_PROC,
        .atime = now,
        .ctime = now,
        .mtime = now,
        .proc.pid = pid,
      };
      return true;
    }
    if (*p != '/') { return false; }
    ++p;
  }
  enum ramfs_device device;
  if (!proc_kind_for_name(p, &device)) { return false; }
  return proc_pid_file_node(pid, device, out);
}

bool vfs_proc_readlink(const char *path, char *out, size_t cap, size_t *len_out) {
  struct vfs_node node;
  if (vfs_proc_lookup(path, &node)) {
    if (node.device == RAMFS_DEV_PROC_PID_FD && node.proc.fd >= 0) {
      char target[VFS_PROC_FD_PATH_MAX];
      size_t target_len = 0;
      if (!fd_link_target(node.proc.fd, target, sizeof(target), &target_len)) { return false; }
      if (len_out != NULL) { *len_out = target_len; }
      size_t copy_len = target_len;
      if (copy_len > cap) { copy_len = cap; }
      if (copy_len != 0) { kmemcpy(out, target, copy_len); }
      return true;
    }
    if (node.device == RAMFS_DEV_PROC_PID_CWD || node.device == RAMFS_DEV_PROC_PID_EXE) {
      return proc_pid_link_target(&node, out, cap, len_out);
    }
  }
  return false;
}

bool vfs_proc_next_dirent(const struct vfs_node *dir, uint64_t *cursor, struct vfs_dirent *out) {
  if (cursor == NULL) { return false; }
  if (dir->backend == VFS_PROC) {
    if (dir->device == RAMFS_DEV_PROC_PID_FD && dir->proc.fd < 0) {
      if (dir->proc.pid != cell_current_pid()) { return false; }
      if (*cursor >= VFS_PROC_FD_MAX) { return false; }
      for (int fd = (int)*cursor; fd < (int)VFS_PROC_FD_MAX; ++fd) {
        if (cell_fd_get_fd_flags(fd) < 0) { continue; }
        *out = (struct vfs_dirent){.ino = 150000u + (uint64_t)dir->proc.pid * 64u + (uint64_t)fd,
                                   .type = VFS_DT_LNK};
        utoa_dec((uint64_t)fd, out->name, sizeof(out->name));
        *cursor = (uint64_t)fd + 1;
        return true;
      }
      return false;
    }
    if (dir->proc.pid < 0) {
      if (*cursor != 0) { return false; }
      int pid = -dir->proc.pid;
      *out = (struct vfs_dirent){.ino = 130000u + (uint64_t)pid, .type = VFS_DT_DIR};
      utoa_dec((uint64_t)pid, out->name, sizeof(out->name));
      *cursor = 1;
      return true;
    }
    static const struct {
      const char *name;
      uint8_t type;
    } entries[] = {{"stat", VFS_DT_REG},     {"status", VFS_DT_REG}, {"cmdline", VFS_DT_REG},
                   {"statm", VFS_DT_REG},    {"comm", VFS_DT_REG},   {"mounts", VFS_DT_REG},
                   {"mountinfo", VFS_DT_REG}, {"cwd", VFS_DT_LNK},    {"exe", VFS_DT_LNK},
                   {"fd", VFS_DT_DIR},       {"task", VFS_DT_DIR}};
    if (!dir->is_dir || *cursor >= sizeof(entries) / sizeof(entries[0])) { return false; }
    const size_t index = (size_t)*cursor;
    *out = (struct vfs_dirent){.ino = dir->ino + index + 1,
                               .type = entries[index].type};
    kmemcpy(out->name, entries[index].name, kstrlen(entries[index].name) + 1);
    ++*cursor;
    return true;
  }
  if (dir->backend == VFS_RAMFS && dir->ramfs.mount == RAMFS_MOUNT_PROC && str_eq(dir->ramfs.name, "proc")) {
    if (*cursor < RAMFS_MAX_NODES) { return false; }
    for (size_t slot = (size_t)(*cursor - RAMFS_MAX_NODES); slot < cell_domain_capacity(); ++slot) {
      struct domain *domain = cell_domain_slot(slot);
      if (domain == NULL || !domain->used) { continue; }
      *out = (struct vfs_dirent){.ino = 100000u + (uint64_t)domain->id, .type = VFS_DT_DIR};
      utoa_dec((uint64_t)domain->id, out->name, sizeof(out->name));
      *cursor = RAMFS_MAX_NODES + slot + 1;
      return true;
    }
  }
  return false;
}
