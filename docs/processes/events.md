# Pipes and waiting for I/O

Anonymous pipes and named FIFOs carry bytes through a page ring. A blocking reader can
sleep until data arrives or the writers close. A blocking writer can sleep until space
opens up. An eventfd carries a 64-bit counter. A read drains the counter, or takes one
in semaphore mode. [Pipes and FIFOs](../../kernel/proc/fd/pipe.c) · [Event counters](../../kernel/proc/events/eventfd.c)

Poll and select check readiness across a supplied descriptor set. An epoll descriptor
keeps its interest list across calls. When a watched file changes state, epoll can wake
a thread waiting for events. [Poll and select](../../kernel/proc/events/poll.c) · [Epoll](../../kernel/proc/events/epoll.c)

[File descriptors](descriptors.md) · [Filesystem notifications](../files/notify.md) · [Thread waits](threads.md)
