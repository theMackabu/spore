#include "fs/procfs.h"
#include "fs/procfs_format.h"
#include "fs/vfs.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "proc/accounting.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/thread.h"

static const struct proc_entry {
  const char *name;
  enum ramfs_device device;
  uint8_t type;
} entries[] = {
  {"stat", RAMFS_DEV_PROC_PID_STAT, VFS_DT_REG},
  {"status", RAMFS_DEV_PROC_PID_STATUS, VFS_DT_REG},
  {"cmdline", RAMFS_DEV_PROC_PID_CMDLINE, VFS_DT_REG},
  {"statm", RAMFS_DEV_PROC_PID_STATM, VFS_DT_REG},
  {"comm", RAMFS_DEV_PROC_PID_COMM, VFS_DT_REG},
  {"mounts", RAMFS_DEV_PROC_PID_MOUNTS, VFS_DT_REG},
  {"mountinfo", RAMFS_DEV_PROC_PID_MOUNTINFO, VFS_DT_REG},
  {"cwd", RAMFS_DEV_PROC_PID_CWD, VFS_DT_LNK},
  {"exe", RAMFS_DEV_PROC_PID_EXE, VFS_DT_LNK},
  {"uid_map", RAMFS_DEV_PROC_UID_MAP, VFS_DT_REG},
  {"gid_map", RAMFS_DEV_PROC_GID_MAP, VFS_DT_REG},
  {"setgroups", RAMFS_DEV_PROC_SETGROUPS, VFS_DT_REG},
  {"fd", RAMFS_DEV_PROC_PID_FD, VFS_DT_DIR},
  {"task", RAMFS_DEV_PROC_PID_TASK, VFS_DT_DIR},
};
constexpr size_t ENTRY_COUNT = sizeof(entries) / sizeof(entries[0]);

static_assert(64 + MAX_FDS <= 2048);

static uint64_t proc_ino(int pid, int tid, unsigned slot) {
  return (UINT64_C(1) << 48) | ((uint64_t)(tid ? tid : pid) << 12) | (tid ? 2048u : 0u) | slot;
}

static bool parse_number(const char **path, int *out) {
  const char *p = *path;
  if (*p < '0' || *p > '9') { return false; }
  int value = 0;
  do {
    int digit = *p++ - '0';
    if (value > (INT32_MAX - digit) / 10) { return false; }
    value = value * 10 + digit;
  } while (*p >= '0' && *p <= '9');
  *path = p;
  *out = value;
  return true;
}

static const char *process_link(const struct domain *d, enum ramfs_device device) {
  const char *text = device == RAMFS_DEV_PROC_PID_CWD ? d->cwd : d->exec_path;
  return *text ? text : "-";
}

static bool proc_node(struct vfs_node *out, const struct domain *d, int tid, unsigned slot, enum ramfs_device device,
                      uint8_t type, int fd) {
  uint64_t now = cell_realtime_seconds();
  *out = (struct vfs_node){
    .backend = VFS_PROC,
    .ino = proc_ino(d->id, tid, slot),
    .dev_id = VFS_DEV_PROC,
    .device = device,
    .is_dir = type == VFS_DT_DIR,
    .mode = type == VFS_DT_DIR   ? 0040555u
            : type == VFS_DT_LNK ? 0120777u
                                 : 0100444u,
    .uid = d->uid,
    .gid = d->gid,
    .links_count = 1,
    .atime = now,
    .ctime = now,
    .mtime = now,
    .proc = {.pid = d->id, .tid = tid, .fd = fd},
  };
  if (device == RAMFS_DEV_PROC_UID_MAP || device == RAMFS_DEV_PROC_GID_MAP || device == RAMFS_DEV_PROC_SETGROUPS) {
    out->mode = 0100644u;
  }
  if (device == RAMFS_DEV_PROC_PID_CWD || device == RAMFS_DEV_PROC_PID_EXE) {
    out->size = kstrlen(process_link(d, device));
  }
  return true;
}

static bool live_task(const struct domain *d, int tid) {
  const struct thread *thread = cell_find_thread(tid);
  return thread != NULL && thread->domain == d;
}

static size_t self_target(int pid, int tid, char *out, size_t cap) {
  size_t len = 0;
  proc_append_u64(out, cap, &len, pid);
  if (tid) {
    proc_append_str(out, cap, &len, "/task/");
    proc_append_u64(out, cap, &len, tid);
  }
  return len;
}

bool vfs_proc_lookup(const char *path, bool follow_self, struct vfs_node *out) {
  if (!starts_with(path, "/proc/")) { return false; }
  const char *p = path + 6;
  int pid, tid = 0;
  bool self = false;
  if (starts_with(p, "self") && (p[4] == '/' || p[4] == '\0')) {
    p += 4;
    self = true;
    pid = cell_current_pid();
  } else if (starts_with(p, "thread-self") && (p[11] == '/' || p[11] == '\0')) {
    p += 11;
    self = true;
    pid = cell_current_pid();
    tid = cell_current_tid();
  } else if (!parse_number(&p, &pid)) {
    return false;
  }
  const struct domain *d = cell_find_domain(pid);
  if (d == NULL) {
    const struct thread *t = cell_find_thread(pid);
    if (t == NULL || t->domain == NULL) { return false; }
    d = t->domain;
    tid = t->tid;
  }
  if (self && !follow_self && *p == '\0') {
    char target[32];
    proc_node(out, d, tid, 0, RAMFS_DEV_PROC_SELF, VFS_DT_LNK, -1);
    out->ino = proc_ino(0, 0, tid ? 2 : 1);
    out->size = self_target(pid, tid, target, sizeof(target));
    return true;
  }
  if (*p == '/') {
    ++p;
    if (tid == 0 && starts_with(p, "task/")) {
      p += 5;
      if (!parse_number(&p, &tid) || !live_task(d, tid)) { return false; }
      if (*p != '/' && *p != '\0') { return false; }
      if (*p == '/') { ++p; }
    }
    if (starts_with(p, "fd/")) {
      p += 3;
      int fd;
      if (!parse_number(&p, &fd) || *p != '\0' || d->id != cell_current_pid() || cell_fd_get_fd_flags(fd) < 0) {
        return false;
      }
      return proc_node(out, d, tid, 64u + (unsigned)fd, RAMFS_DEV_PROC_PID_FD, VFS_DT_LNK, fd);
    }
    for (size_t i = 0; i < ENTRY_COUNT; ++i) {
      const struct proc_entry *e = &entries[i];
      if ((tid == 0 || e->device != RAMFS_DEV_PROC_PID_TASK) && str_eq(p, e->name)) {
        return proc_node(out, d, tid, (unsigned)i + 1, e->device, e->type, -1);
      }
    }
  }
  return *p == '\0' && proc_node(out, d, tid, 0, RAMFS_DEV_NONE, VFS_DT_DIR, -1);
}

bool vfs_proc_node_live(const struct vfs_node *node) {
  const struct domain *d = cell_find_domain(node->proc.pid);
  if (d == NULL || (node->proc.tid != 0 && !live_task(d, node->proc.tid))) { return false; }
  if (node->device == RAMFS_DEV_PROC_PID_FD && node->proc.fd >= 0) {
    return d->id == cell_current_pid() && cell_fd_get_fd_flags(node->proc.fd) >= 0;
  }
  return true;
}

static size_t fd_link_target(int fd, char *out, size_t cap) {
  if (cell_fd_path(fd, out, cap)) { return kstrlen(out); }
  struct domain *d = cell_current_domain_internal();
  struct open_file *file = cell_file_at(d, fd);
  struct vfs_node node;
  size_t len = 0;
  if ((file->type == OPEN_PIPE || file->type == OPEN_SOCKET || file->type == OPEN_UNIX_STREAM ||
       file->type == OPEN_UNIX_LISTENER) &&
      cell_fd_stat(fd, &node)) {
    proc_append_str(out, cap, &len, file->type == OPEN_PIPE ? "pipe:[" : "socket:[");
    proc_append_u64(out, cap, &len, node.ino);
  } else {
    proc_append_str(out, cap, &len, "anon_inode:[fd");
    proc_append_u64(out, cap, &len, (uint64_t)fd);
  }
  proc_append_char(out, cap, &len, ']');
  return len;
}

bool vfs_proc_readlink(const struct vfs_node *node, char *out, size_t cap, size_t *len_out) {
  if (!vfs_proc_node_live(node) || (node->mode & 0170000u) != 0120000u) { return false; }
  char buf[FD_PATH_MAX];
  const char *target = buf;
  size_t len;
  if (node->device == RAMFS_DEV_PROC_SELF) {
    len = self_target(node->proc.pid, node->proc.tid, buf, sizeof(buf));
  } else if (node->device == RAMFS_DEV_PROC_PID_FD) {
    len = fd_link_target(node->proc.fd, buf, sizeof(buf));
  } else {
    target = process_link(cell_find_domain(node->proc.pid), node->device);
    len = kstrlen(target);
  }
  if (len_out != NULL) { *len_out = len; }
  if (cap != 0) { kmemcpy(out, target, len < cap ? len : cap); }
  return true;
}

static bool numbered_dirent(struct vfs_dirent *out, uint64_t ino, uint8_t type, unsigned number) {
  size_t len = 0;
  proc_append_u64(out->name, sizeof(out->name), &len, number);
  out->name_len = (uint8_t)len;
  out->ino = ino;
  out->type = type;
  return true;
}

bool vfs_proc_next_dirent(const struct vfs_node *dir, uint64_t *cursor, struct vfs_dirent *out) {
  if (dir->backend == VFS_RAMFS && dir->ramfs.mount == RAMFS_MOUNT_PROC && str_eq(dir->ramfs.name, "proc")) {
    if (*cursor < RAMFS_DIR_END) { return false; }
    if (*cursor < RAMFS_DIR_END + 2) {
      bool thread = *cursor == RAMFS_DIR_END + 1;
      const char *name = thread ? "thread-self" : "self";
      out->ino = proc_ino(0, 0, thread ? 2 : 1);
      out->type = VFS_DT_LNK;
      out->name_len = (uint8_t)kstrlen(name);
      kmemcpy(out->name, name, (size_t)out->name_len + 1);
      ++*cursor;
      return true;
    }
    for (uint64_t i = *cursor - RAMFS_DIR_END - 2, end = cell_domain_slot_limit(); i < end; ++i) {
      const struct domain *d = cell_domain_slot(i);
      if (!d->used) { continue; }
      *cursor = RAMFS_DIR_END + 2 + i + 1;
      return numbered_dirent(out, proc_ino(d->id, 0, 0), VFS_DT_DIR, d->id);
    }
    return false;
  }
  if (dir->backend != VFS_PROC || !dir->is_dir) { return false; }
  int pid = dir->proc.pid, tid = dir->proc.tid;
  if (dir->device == RAMFS_DEV_PROC_PID_FD) {
    if (pid != cell_current_pid()) { return false; }
    if (*cursor >= MAX_FDS) { return false; }
    int fd = cell_next_open_fd(cell_current_domain_internal(), (unsigned)*cursor);
    if (fd >= 0) {
      *cursor = (unsigned)fd + 1;
      return numbered_dirent(out, proc_ino(pid, tid, 64u + (unsigned)fd), VFS_DT_LNK, (unsigned)fd);
    }
    return false;
  }
  if (dir->device == RAMFS_DEV_PROC_PID_TASK) {
    for (uint64_t i = *cursor, end = cell_thread_slot_limit(); i < end; ++i) {
      const struct thread *t = cell_thread_slot(i);
      if (t->state == THREAD_UNUSED || t->domain == NULL || t->domain->id != pid) { continue; }
      *cursor = i + 1;
      return numbered_dirent(out, proc_ino(pid, t->tid, 0), VFS_DT_DIR, t->tid);
    }
    return false;
  }
  if (*cursor >= ENTRY_COUNT) { return false; }
  const struct proc_entry *e = &entries[*cursor];
  if (tid && e->device == RAMFS_DEV_PROC_PID_TASK) { return false; }
  out->ino = proc_ino(pid, tid, (unsigned)*cursor + 1);
  out->type = e->type;
  out->name_len = (uint8_t)kstrlen(e->name);
  kmemcpy(out->name, e->name, (size_t)out->name_len + 1);
  ++*cursor;
  return true;
}
