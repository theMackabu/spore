#include "internal.h"
#include "abi/linux/aarch64/open.h"
#include "fs/vfs.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "proc/fd.h"

static struct open_file *names[128];
static unsigned open_names;
static uint32_t rename_cookie;

static struct open_file **bucket(uint64_t dev, uint64_t ino) {
  return &names[(dev ^ ino) % (sizeof(names) / sizeof(*names))];
}

static bool locate(const char *path, struct fsnotify_name *location) {
  if (!path) { return false; }
  size_t end = kstrlen(path);
  while (end && path[end - 1] == '/') { --end; }
  size_t start = end;
  while (start && path[start - 1] != '/') { --start; }
  if (!start || end - start >= sizeof(location->name)) { return false; }
  char parent[CELL_PATH_MAX];
  size_t prefix = start == 1 ? 1 : start - 1;
  if (prefix >= sizeof(parent)) { return false; }
  kmemcpy(parent, path, prefix);
  parent[prefix] = 0;
  struct vfs_node node;
  if (!vfs_lookup(parent, &node)) { return false; }
  location->parent_dev = node.dev_id;
  location->parent_ino = node.ino;
  kmemcpy(location->name, path + start, end - start);
  location->name[end - start] = 0;
  return true;
}

static void parent_event(const struct fsnotify_name *location, uint32_t mask, uint32_t cookie, bool unlinked) {
  if (location->parent_ino) {
    fsnotify_emit(location->parent_dev, location->parent_ino, mask, cookie, location->name, unlinked);
  }
}

static void resolve_open_name(struct open_file *file) {
  if (!file->name_unresolved) { return; }
  struct fsnotify_name location = {0};
  locate(file->notify_name.name, &location);
  file->notify_name = location;
  file->name_unresolved = false;
}

void fsnotify_file_event(struct open_file *file, uint32_t mask) {
  if (!fsnotify_active() || (file->type != OPEN_VFS && file->type != OPEN_PIPE) || !file->node.ino) { return; }
  if (file->node.is_dir) { mask |= FS_ISDIR; }
  fsnotify_emit(file->node.dev_id, file->node.ino, mask, 0, NULL, false);
  if (fsnotify_directories()) {
    resolve_open_name(file);
    parent_event(&file->notify_name, mask, 0, file->name_unlinked);
  }
  inotify_flush();
}

void fsnotify_track_open(struct open_file *file, const char *resolved) {
  if (file->flags & LINUX_O_PATH) { return; }
  if (resolved && resolved[0] && !str_eq(resolved, "/")) {
    copy_cstr(file->notify_name.name, sizeof(file->notify_name.name), resolved);
    file->name_unresolved = true;
    struct open_file **head = bucket(file->node.dev_id, file->node.ino);
    file->name_next = *head;
    file->name_link = head;
    if (*head) { (*head)->name_link = &file->name_next; }
    *head = file;
    ++open_names;
  }
}

void fsnotify_opened(struct open_file *file) {
  if (file->flags & LINUX_O_PATH) { return; }
  file->open_notified = true;
  fsnotify_file_event(file, FS_OPEN);
}

void fsnotify_closed(struct open_file *file) {
  if (!file->node.ino || (file->flags & LINUX_O_PATH)) { return; }
  if (file->open_notified) { fsnotify_file_event(file, linux_open_writable(file->flags) ? FS_CLOSE_WRITE : FS_CLOSE_NOWRITE); }
  if (file->name_link) {
    *file->name_link = file->name_next;
    if (file->name_next) { file->name_next->name_link = file->name_link; }
    --open_names;
  }
}

void fsnotify_capture(const char *path, struct fsnotify_entry *entry) {
  entry->ino = entry->location.parent_ino = 0;
  if (!open_names && !fsnotify_active()) { return; }
  struct vfs_node node;
  if (vfs_lstat(path, &node)) {
    entry->dev = node.dev_id;
    entry->ino = node.ino;
    entry->is_dir = node.is_dir;
  }
  locate(path, &entry->location);
}

void fsnotify_path_event(const char *path, uint32_t mask) {
  if (!fsnotify_active()) { return; }
  struct fsnotify_entry entry;
  fsnotify_capture(path, &entry);
  if (!entry.ino) { return; }
  if (entry.is_dir) { mask |= FS_ISDIR; }
  parent_event(&entry.location, mask, 0, false);
  if (!(mask & FS_CREATE)) { fsnotify_emit(entry.dev, entry.ino, mask, 0, NULL, false); }
  inotify_flush();
}

void fsnotify_linked(const char *path) {
  if (!fsnotify_active()) { return; }
  struct fsnotify_entry entry;
  fsnotify_capture(path, &entry);
  if (!entry.ino) { return; }
  fsnotify_emit(entry.dev, entry.ino, FS_ATTRIB, 0, NULL, false);
  parent_event(&entry.location, FS_CREATE, 0, false);
  inotify_flush();
}

void fsnotify_prepare_change(const struct fsnotify_entry *entry) {
  if (!entry->ino) { return; }
  if (entry->is_dir) {
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
      for (struct open_file *f = names[i]; f; f = f->name_next) { resolve_open_name(f); }
    }
  } else {
    for (struct open_file *f = *bucket(entry->dev, entry->ino); f; f = f->name_next) {
      if (f->node.dev_id == entry->dev && f->node.ino == entry->ino) { resolve_open_name(f); }
    }
  }
}

static void relabel(const struct fsnotify_entry *entry, const struct fsnotify_name *to) {
  if (!entry->ino) { return; }
  for (struct open_file *f = *bucket(entry->dev, entry->ino); f; f = f->name_next) {
    if (f->node.ino != entry->ino || f->node.dev_id != entry->dev || f->name_unlinked ||
        f->notify_name.parent_dev != entry->location.parent_dev || f->notify_name.parent_ino != entry->location.parent_ino ||
        !str_eq(f->notify_name.name, entry->location.name)) { continue; }
    if (to) { f->notify_name = *to; }
    else { f->name_unlinked = true; }
  }
}

void fsnotify_removed(const struct fsnotify_entry *entry) {
  if (!entry->ino) { return; }
  relabel(entry, NULL);
  if (fsnotify_active()) {
    parent_event(&entry->location, FS_DELETE | (entry->is_dir ? FS_ISDIR : 0), 0, false);
    fsnotify_emit(entry->dev, entry->ino, FS_ATTRIB, 0, NULL, false);
  }
  inotify_flush();
}

void fsnotify_renamed(const struct fsnotify_entry *old, const struct fsnotify_entry *to) {
  if (!old->ino || (old->ino == to->ino && old->dev == to->dev)) { return; }
  relabel(to, NULL);
  relabel(old, &to->location);
  if (fsnotify_active()) {
    if (!++rename_cookie) { ++rename_cookie; }
    uint32_t type = old->is_dir ? FS_ISDIR : 0;
    parent_event(&old->location, FS_MOVED_FROM | type, rename_cookie, false);
    parent_event(&to->location, FS_MOVED_TO | type, rename_cookie, false);
    fsnotify_emit(old->dev, old->ino, FS_MOVE_SELF, 0, NULL, false);
    if (to->ino) { fsnotify_emit(to->dev, to->ino, FS_ATTRIB, 0, NULL, false); }
  }
  inotify_flush();
}
