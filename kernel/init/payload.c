#include "boot.h"
#include "fs/ramfs.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "proc/limits.h"
#include "net/net.h"

struct payload_reader {
  const uint8_t *next;
  size_t remaining;
};

static const void *take(struct payload_reader *reader, size_t n) {
  if (n > reader->remaining) { return NULL; }
  const uint8_t *p = reader->next;
  reader->next += n;
  reader->remaining -= n;
  return p;
}

static const char *take_string(struct payload_reader *reader) {
  size_t n = 0;
  while (n < reader->remaining && reader->next[n]) { ++n; }
  return n == reader->remaining ? NULL : take(reader, n + 1);
}

static bool path_ok(const char *p) {
  if (!p || *p != '/' || kstrlen(p) >= CELL_PATH_MAX) { return false; }
  while (*p) {
    const char *part = ++p;
    while (*p && *p != '/') {
      ++p;
    }
    size_t n = p - part;
    if (!n || (n == 1 && *part == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) { return false; }
  }
  return true;
}

static bool unpack_entry(struct payload_reader *reader, struct ramfs *fs, const char *executable) {
  const char *path = take_string(reader);
  const void *size = take(reader, sizeof(uint64_t));
  if (!path_ok(path) || !size) { return false; }
  uint64_t n;
  kmemcpy(&n, size, sizeof(n));
  if (n == UINT64_MAX) { return ramfs_mkdir(fs, path, 0555); }
  if (n == UINT64_MAX - 1) {
    const char *source = take_string(reader);
    return path_ok(source) && ramfs_link(fs, source, path);
  }
  const void *data = take(reader, n);
  return data && ramfs_add_boot_file(fs, path, data, n, str_eq(path, executable) ? 0555 : 0444);
}

bool app_unpack(const void *data, size_t size, struct ramfs *fs, struct init_program *program, struct net_config *network) {
  struct payload_reader reader = {data, size};
  const char *magic = take(&reader, 8);
  struct {
    uint32_t argc, envc, entries, ip, gateway, mask, dns;
  } header;
  const void *bytes = take(&reader, sizeof(header));
  if (!magic || kmemcmp(magic, "SPOREAP1", 8) || !bytes) { return false; }
  kmemcpy(&header, bytes, sizeof(header));
  if (!header.argc || header.argc > INIT_MAX_ARGS || header.envc > INIT_MAX_ENV) { return false; }
  program->path = take_string(&reader);
  program->cwd = take_string(&reader);
  if (!path_ok(program->path) || !program->cwd || (!str_eq(program->cwd, "/") && !path_ok(program->cwd))) {
    return false;
  }
  program->argc = header.argc;
  program->envc = header.envc;
  for (unsigned i = 0; i < program->argc; ++i) {
    if (!(program->argv[i] = take_string(&reader))) { return false; }
  }
  for (unsigned i = 0; i < program->envc; ++i) {
    if (!(program->env[i] = take_string(&reader))) { return false; }
  }
  for (unsigned i = 0; i < header.entries; ++i) {
    if (!unpack_entry(&reader, fs, program->path)) { return false; }
  }
  *network = (struct net_config){header.ip, header.gateway, header.mask, header.dns, 1};
  return reader.remaining == 0;
}
