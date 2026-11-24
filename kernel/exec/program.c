#include "exec/elf.h"

constexpr uint64_t EXEC_LOAD_BASE = 0x0000004000000000ull;
constexpr uint64_t INTERP_LOAD_BASE = 0x0000006000000000ull;

enum elf_load_status elf_load_program(struct user_address_space *as, struct vma_list *vmas,
                                      const struct elf_reader *reader, struct loaded_elf *out, char *interp_path,
                                      size_t path_cap) {
  if (interp_path == NULL || !elf_map_image(vmas, reader, EXEC_LOAD_BASE, out, interp_path, path_cap)) {
    return ELF_BAD_EXECUTABLE;
  }
  as->freebsd_abi = out->osabi == 9;
  if (interp_path[0] != '\0') {
    if (as->freebsd_abi) { return ELF_BAD_INTERPRETER; }
    struct vfs_node node;
    struct elf_reader loader;
    struct loaded_elf interp;
    if (!vfs_lookup(interp_path, &node) || !elf_reader_from_vfs(&node, &loader) ||
        !elf_map_image(vmas, &loader, INTERP_LOAD_BASE, &interp, NULL, 0) || interp.osabi == 9) {
      return ELF_BAD_INTERPRETER;
    }
    out->runtime_entry = interp.entry;
    out->at_base = interp.load_base;
  }
  as->brk_base = out->brk_base;
  as->brk_current = out->brk_base;
  return ELF_LOADED;
}
