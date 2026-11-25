#include "init/boot.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/pipe.h"

__attribute__((import_module("spore"), import_name("stdin_read"))) int host_stdin_read(void *, size_t);

static struct pipe_obj *pipe;
static uint8_t buffer[4096];
static size_t pending;

void wasm_open_input(struct domain *domain) {
  pipe = cell_pipe_create();
  struct open_file *in = cell_alloc_open_file();
  if (!pipe || !in) { boot_fail(BOOT_INIT_FAILED, "cannot allocate stdin pipe"); }
  cell_pipe_add_reader(pipe);
  cell_pipe_add_writer(pipe);
  in->type = OPEN_PIPE;
  in->pipe = pipe;
  cell_close_domain_fd(domain, 0);
  cell_assign_fd(domain, 0, in, false);
}

static int copy_input(void *context, uint8_t *out, size_t len) {
  const uint8_t **src = context;
  kmemcpy(out, *src, len);
  *src += len;
  return 0;
}

void wasm_poll_input(void) {
  if (!pipe) { return; }
  for (unsigned i = 0; i < 16; ++i) {
    if (!cell_pipe_has_readers(pipe)) { goto close; }
    if (!pending) {
      int n = host_stdin_read(buffer, sizeof(buffer));
      if (n < 0) { goto close; }
      if (!n) { return; }
      if ((size_t)n > sizeof(buffer)) { boot_fail(BOOT_INIT_FAILED, "invalid host stdin length"); }
      pending = (size_t)n;
    }
    const uint8_t *p = buffer;
    int64_t n = cell_pipe_write_data(pipe, pending, copy_input, &p, NULL);
    if (n == -11) { return; }
    if (n != (int64_t)pending) { goto close; }
    pending = 0;
  }
  return;
close:
  cell_pipe_drop_writer(pipe);
  pipe = NULL;
  cell_pipe_notify();
}
