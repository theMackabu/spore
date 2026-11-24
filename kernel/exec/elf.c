#include "exec/elf.h"
#include "elf_rust.h"

#include "lib/mem.h"

enum { PF_X = 1, PF_W = 2 };

static bool bounds_ok(uint64_t off, uint64_t len, uint64_t image_size) {
  return off <= image_size && len <= image_size - off;
}

static bool read_exact(const struct elf_reader *reader, uint64_t off, void *dst, size_t len) {
  return reader != NULL && reader->read_at != NULL && bounds_ok(off, len, reader->size) &&
         reader->read_at(reader->ctx, off, dst, len);
}

static uint32_t phdr_to_vmm_flags(uint32_t flags) {
  uint32_t out = VMM_USER_READ;
  if ((flags & PF_W) != 0) { out |= VMM_USER_WRITE; }
  if ((flags & PF_X) != 0) { out |= VMM_USER_EXEC; }
  return out;
}

static bool rust_read(void *ctx, uint64_t off, void *dst, size_t len) {
  return read_exact(ctx, off, dst, len);
}

bool elf_find_interp_aarch64(const struct elf_reader *reader, char *out, size_t out_cap) {
  return reader != NULL && spore_elf_find_interp(rust_read, (void *)reader, reader->size, out, out_cap);
}

struct elf_map_context { struct vma_list *vmas; const struct elf_reader *reader; };
static bool rust_map(void *ctx, const struct spore_elf_program *program, uint64_t segment, uint64_t start, uint64_t end) {
  struct elf_map_context *map = ctx;
  return map != NULL && program != NULL && vma_insert_file(map->vmas, start, end, phdr_to_vmm_flags(program->flags), 0,
                                                           map->reader->node, segment, program->offset, program->filesz);
}

bool elf_load_aarch64(struct user_address_space *as, struct vma_list *vmas, const struct elf_reader *reader,
                      uint64_t load_base, struct loaded_elf *out) {
  if (as == NULL || vmas == NULL || reader == NULL || reader->node == NULL) {
    return false;
  }
  struct elf_map_context map = {.vmas = vmas, .reader = reader};
  struct spore_elf_loaded loaded;
  if (!spore_elf_load(rust_read, (void *)reader, reader->size, load_base, rust_map, &map, &loaded)) { return false; }
  out->entry = loaded.entry;
  out->runtime_entry = out->entry;
  out->load_base = loaded.load_base;
  out->at_base = 0;
  out->phdr = loaded.phdr;
  out->phent = loaded.phent;
  out->phnum = loaded.phnum;
  out->brk_base = loaded.brk_base;
  as->brk_base = out->brk_base;
  as->brk_current = out->brk_base;
  return true;
}
