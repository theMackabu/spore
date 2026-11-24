#include "exec/elf.h"
#include "elf_rust.h"

enum { PF_X = 1, PF_W = 2 };

static uint32_t phdr_to_vmm_flags(uint32_t flags) {
  uint32_t out = VMM_USER_READ;
  if ((flags & PF_W) != 0) { out |= VMM_USER_WRITE; }
  if ((flags & PF_X) != 0) { out |= VMM_USER_EXEC; }
  return out;
}

struct elf_map_context {
  struct vma_list *vmas;
  const struct elf_reader *reader;
};
static bool map_segment(void *ctx, const struct spore_elf_program *program, uint64_t segment, uint64_t start,
                        uint64_t end) {
  struct elf_map_context *map = ctx;
  return map != NULL && program != NULL &&
         vma_insert_file(map->vmas, start, end, phdr_to_vmm_flags(program->flags), 0, map->reader->node, segment,
                         program->offset, program->filesz);
}

bool elf_map_image(struct vma_list *vmas, const struct elf_reader *reader, uint64_t load_base, struct loaded_elf *out,
                   char *interp_path, size_t path_cap) {
  if (vmas == NULL || reader == NULL || reader->node == NULL || out == NULL) { return false; }
  struct elf_map_context map = {.vmas = vmas, .reader = reader};
  return spore_elf_load(reader->read_at, reader->ctx, reader->size, load_base, map_segment, &map, out, interp_path,
                        path_cap);
}
