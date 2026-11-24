# Watching a directory

An inotify descriptor owns watches and a queue of events. A watch matches changes to a
device and inode. For a rename, the old and new parent directories receive move events
with a shared cookie, so a reader watching both can match the names. [Watch matching](../../kernel/fs/notify/watch.c) · [Rename events](../../kernel/fs/notify/names.c)

A read drains whole records. Adjacent identical records can coalesce. When the queue runs
out of space it records an overflow event. A blocking read can sleep on an empty queue,
and poll or epoll can report when events are ready. [Event queue](../../kernel/fs/notify/queue.c) · [Guest notification tests](../../userland/tests/integration/notify.c)

[I/O readiness](../processes/events.md) · [Filesystem routing](vfs.md)
