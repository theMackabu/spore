#pragma once
#include "fs/notify.h"
#include <stddef.h>

struct inotify_watch;
struct inotify_event_record {
  struct inotify_event_record *next;
  struct {
    int32_t wd;
    uint32_t mask, cookie, len;
    char name[256];
  } event;
};

struct inotify_state {
  struct open_file *file;
  struct inotify_watch *watches;
  uint32_t next_wd, count, bytes;
  struct inotify_event_record *head, *tail;
  struct inotify_event_record overflow;
  struct inotify_state *pending_next;
  bool pending, overflow_queued, wd_wrapped;
};

enum { FS_IGNORED = 0x8000, FS_ISDIR = 1U << 30 };
void inotify_enqueue(struct inotify_state *state, int wd, uint32_t mask, uint32_t cookie, const char *name);
void inotify_clear_events(struct inotify_state *state);
void inotify_flush(void);

bool fsnotify_directories(void);
