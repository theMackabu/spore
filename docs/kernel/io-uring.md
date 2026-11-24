# io_uring

AArch64, x86-64 and RV64 share one Linux io_uring implementation. Applications can discover operations with `IORING_REGISTER_PROBE` and read the setup feature bits before choosing a submission path. [Kernel module](../../kernel/io_uring/) · [Opcode table](../../kernel/io_uring/operations.def)

A submission can complete immediately or wait for data, descriptor readiness or a deadline. Pending file operations retain the underlying open file so closing and reusing its descriptor cannot redirect the request. Completions include the application's `user_data`. If the completion queue fills, the kernel keeps overflow entries for a later `io_uring_enter`. [Submission and completion](../../kernel/io_uring/queue.c) · [Pending requests](../../kernel/io_uring/pending.c)

For regular files, registered buffers pin the backing pages and fixed reads and writes can transfer contiguous physical ranges directly. Registered files, sparse resource tables, resource updates and retirement tags are supported. Services can use provided-buffer groups or register a userspace buffer ring for reads and receives. [Registered buffers](../../kernel/io_uring/buffers.c) · [Buffer groups](../../kernel/io_uring/groups.c) · [Registration](../../kernel/io_uring/register.c)

Compatibility is partial. SQPOLL submission threads, device IOPOLL, driver-specific commands and zero-copy transmit notifications remain unimplemented. Support for individual SQE flags and linked operations varies by opcode. In particular, operations that require a worker to execute an arbitrary filesystem syscall do not have a general worker backend.

Run `spore test ports` to exercise the interface in all three guests. In the development shell, `spore bench uring` measures NOP batches of 1, 8 and 64 submissions and compares 64-byte tmpfs reads using pread, batched reads and registered resources. The read benchmark includes checking each result. [Guest tests](../../tests/appliance/uring.c) · [Benchmark](../../tests/appliance/uring-bench.c)
