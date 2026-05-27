#include "arch/aarch64/syscall_handlers.h"

#include "cell.h"
#include "elf/loader.h"
#include "exec/stack.h"
#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  EFAULT = 14,
  EACCES = 13,
  EINVAL = 22,
  ENOENT = 2,
  EPERM = 1,
  X_OK = 1,
  MAX_EXEC_ARGS = 8,
  MAX_EXEC_ENVS = 64,
  MAX_EXEC_STRING = 8192,
};

#define INTERP_LOAD_BASE 0x0000006000000000ull

static bool streq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) { return false; }
  }
  return *a == '\0' && *b == '\0';
}

static bool copy_exec_path(uint64_t path_addr, char *out, size_t cap) {
  char raw[128];
  char virtual_path[128];
  if (!syscall_copy_string_from_user(path_addr, raw, sizeof(raw)) ||
      !syscall_normalize_path(cell_current_cwd(), raw, virtual_path, sizeof(virtual_path))) {
    return false;
  }
  const char *chroot = cell_current_chroot();
  if (streq(chroot, "/")) {
    if (kstrlen(virtual_path) >= cap) { return false; }
    kmemcpy(out, virtual_path, kstrlen(virtual_path) + 1);
    return true;
  }
  if (streq(virtual_path, "/")) {
    if (kstrlen(chroot) >= cap) { return false; }
    kmemcpy(out, chroot, kstrlen(chroot) + 1);
    return true;
  }
  size_t root_len = kstrlen(chroot);
  size_t path_len = kstrlen(virtual_path);
  if (root_len + path_len >= cap) { return false; }
  kmemcpy(out, chroot, root_len);
  kmemcpy(out + root_len, virtual_path, path_len + 1);
  return true;
}

static bool copy_string_array_from_user(uint64_t user, char store[][MAX_EXEC_STRING], const char *ptrs[],
                                        uint64_t max_count, uint64_t *out_count) {
  *out_count = 0;
  if (user == 0) { return true; }
  for (uint64_t i = 0; i < max_count; ++i) {
    uint64_t ptr;
    if (!syscall_user_readable(user + i * sizeof(uint64_t), sizeof(ptr)) ||
        !vmm_copy_from_user(syscall_active_as(), &ptr, user + i * sizeof(uint64_t), sizeof(ptr))) {
      return false;
    }
    if (ptr == 0) {
      *out_count = i;
      return true;
    }
    if (!syscall_copy_string_from_user(ptr, store[i], MAX_EXEC_STRING)) { return false; }
    ptrs[i] = store[i];
  }
  *out_count = max_count;
  return true;
}

static bool copy_kernel_string(char *dst, size_t cap, const char *src) {
  size_t len = kstrlen(src);
  if (len >= cap) { return false; }
  kmemcpy(dst, src, len + 1);
  return true;
}

static bool parse_shebang(const void *data, uint64_t size, char *interp, size_t interp_cap, char *arg, size_t arg_cap,
                          bool *has_arg) {
  const char *text = data;
  *has_arg = false;
  if (size < 3 || text[0] != '#' || text[1] != '!') { return false; }
  size_t i = 2;
  while (i < size && (text[i] == ' ' || text[i] == '\t')) {
    ++i;
  }
  size_t interp_len = 0;
  while (i + interp_len < size && text[i + interp_len] != '\n' && text[i + interp_len] != '\r' &&
         text[i + interp_len] != ' ' && text[i + interp_len] != '\t') {
    if (interp_len + 1 >= interp_cap) { return false; }
    interp[interp_len] = text[i + interp_len];
    ++interp_len;
  }
  if (interp_len == 0) { return false; }
  interp[interp_len] = '\0';
  i += interp_len;
  while (i < size && (text[i] == ' ' || text[i] == '\t')) {
    ++i;
  }
  size_t arg_len = 0;
  while (i + arg_len < size && text[i + arg_len] != '\n' && text[i + arg_len] != '\r') {
    if (arg_len + 1 >= arg_cap) { return false; }
    arg[arg_len] = text[i + arg_len];
    ++arg_len;
  }
  while (arg_len > 0 && (arg[arg_len - 1] == ' ' || arg[arg_len - 1] == '\t')) {
    --arg_len;
  }
  arg[arg_len] = '\0';
  *has_arg = arg_len != 0;
  return true;
}

static bool vfs_elf_read_at(void *ctx, uint64_t offset, void *dst, size_t len) {
  const struct vfs_node *node = ctx;
  return node != NULL && vfs_read(node, offset, dst, len) == len;
}

static bool make_elf_reader(const struct vfs_node *node, struct elf_reader *reader) {
  if (node == NULL || reader == NULL || node->is_dir || node->device != RAMFS_DEV_NONE) { return false; }
  *reader = (struct elf_reader){
    .read_at = vfs_elf_read_at,
    .ctx = (void *)node,
    .node = node,
    .size = node->size,
  };
  return true;
}

static bool parse_shebang_node(const struct vfs_node *node, char *interp, size_t interp_cap, char *arg, size_t arg_cap,
                               bool *has_arg) {
  char head[256];
  uint64_t n = node->size < sizeof(head) ? node->size : sizeof(head);
  if (n == 0 || vfs_read(node, 0, head, n) != n) { return false; }
  return parse_shebang(head, n, interp, interp_cap, arg, arg_cap, has_arg);
}

int64_t sys_execve(struct trap_frame *frame, uint64_t path_addr, uint64_t argv_addr, uint64_t envp_addr) {
  char path[128];
  if (!copy_exec_path(path_addr, path, sizeof(path))) { return -(int64_t)EFAULT; }
  if (!cell_fs_path_allowed(path, CELL_FS_EXEC)) { return -(int64_t)EPERM; }

  // Large argv/env scratch is static because the v2 run-to-completion kernel
  // never re-enters sys_execve concurrently on this single CPU.
  static char argv_store[MAX_EXEC_ARGS][MAX_EXEC_STRING];
  static char env_store[MAX_EXEC_ENVS][MAX_EXEC_STRING];
  const char *argv[MAX_EXEC_ARGS];
  const char *envp[MAX_EXEC_ENVS];
  uint64_t argc = 0;
  uint64_t envc = 0;
  if (!copy_string_array_from_user(argv_addr, argv_store, argv, MAX_EXEC_ARGS, &argc) ||
      !copy_string_array_from_user(envp_addr, env_store, envp, MAX_EXEC_ENVS, &envc)) {
    return -(int64_t)EFAULT;
  }
  if (argc == 0) {
    argv_store[0][0] = '\0';
    for (size_t i = 0; i + 1 < sizeof(argv_store[0]) && path[i] != '\0'; ++i) {
      argv_store[0][i] = path[i];
      argv_store[0][i + 1] = '\0';
    }
    argv[0] = argv_store[0];
    argc = 1;
  }

  struct vfs_node exec_node;
  if (!vfs_lookup(path, &exec_node)) { return -(int64_t)ENOENT; }
  if (!syscall_node_access_allowed(&exec_node, X_OK)) { return -(int64_t)EACCES; }
  struct elf_reader exec_reader;
  if (!make_elf_reader(&exec_node, &exec_reader)) { return -(int64_t)ENOENT; }
  static char shebang_store[MAX_EXEC_ARGS][MAX_EXEC_STRING];
  char shebang_interp[128];
  char shebang_arg[128];
  bool shebang_has_arg = false;
  if (parse_shebang_node(&exec_node, shebang_interp, sizeof(shebang_interp), shebang_arg, sizeof(shebang_arg),
                         &shebang_has_arg)) {
    struct vfs_node script_node = exec_node;
    struct vfs_node shebang_node;
    if (!vfs_lookup(shebang_interp, &shebang_node) || !make_elf_reader(&shebang_node, &exec_reader)) {
      return -(int64_t)ENOENT;
    }
    const char *old_argv[MAX_EXEC_ARGS];
    uint64_t old_argc = argc;
    for (uint64_t i = 0; i < old_argc; ++i) {
      old_argv[i] = argv[i];
    }
    uint64_t new_argc = 0;
    if (!copy_kernel_string(shebang_store[new_argc], MAX_EXEC_STRING, shebang_interp)) { return -(int64_t)EINVAL; }
    argv[new_argc] = shebang_store[new_argc];
    ++new_argc;
    if (shebang_has_arg) {
      if (new_argc >= MAX_EXEC_ARGS || !copy_kernel_string(shebang_store[new_argc], MAX_EXEC_STRING, shebang_arg)) {
        return -(int64_t)EINVAL;
      }
      argv[new_argc] = shebang_store[new_argc];
      ++new_argc;
    }
    if (new_argc >= MAX_EXEC_ARGS || !copy_kernel_string(shebang_store[new_argc], MAX_EXEC_STRING, path)) {
      return -(int64_t)EINVAL;
    }
    argv[new_argc] = shebang_store[new_argc];
    ++new_argc;
    for (uint64_t i = 1; i < old_argc && new_argc < MAX_EXEC_ARGS; ++i) {
      if (!copy_kernel_string(shebang_store[new_argc], MAX_EXEC_STRING, old_argv[i])) { return -(int64_t)EINVAL; }
      argv[new_argc] = shebang_store[new_argc];
      ++new_argc;
    }
    argc = new_argc;
    exec_node = shebang_node;
    (void)script_node;
  }
  char interp_path[128];
  bool has_interp = elf_find_interp_aarch64(&exec_reader, interp_path, sizeof(interp_path));

  struct user_address_space new_as;
  struct loaded_elf elf;
  struct vma_list new_vmas;
  uint64_t user_sp = 0;
  uint64_t hhdm_offset = syscall_active_as()->hhdm_offset;
  vma_list_init(&new_vmas);
  if (!vmm_user_init(&new_as, hhdm_offset)) { return -12; }
  if (!elf_load_aarch64(&new_as, &new_vmas, &exec_reader, 0, &elf)) {
    vmm_destroy(&new_as);
    vma_list_destroy(&new_vmas);
    return -(int64_t)EINVAL;
  }
  if (has_interp) {
    struct vfs_node interp_node;
    struct elf_reader interp_reader;
    struct loaded_elf interp;
    if (!vfs_lookup(interp_path, &interp_node) || !make_elf_reader(&interp_node, &interp_reader) ||
        !elf_load_aarch64(&new_as, &new_vmas, &interp_reader, INTERP_LOAD_BASE, &interp)) {
      vmm_destroy(&new_as);
      vma_list_destroy(&new_vmas);
      return -(int64_t)ENOENT;
    }
    elf.runtime_entry = interp.entry;
    elf.at_base = interp.load_base;
  }
  struct exec_stack_credentials creds = {
    .uid = cell_current_uid(),
    .euid = cell_current_euid(),
    .gid = cell_current_gid(),
    .egid = cell_current_egid(),
  };
  if ((exec_node.mode & 04000u) != 0) { creds.euid = exec_node.uid; }
  if ((exec_node.mode & 02000u) != 0) { creds.egid = exec_node.gid; }

  if (!build_initial_stack_args(&new_as, &elf, argv, argc, envp, envc, &creds, &user_sp)) {
    vmm_destroy(&new_as);
    vma_list_destroy(&new_vmas);
    return -(int64_t)EINVAL;
  }
  if (!vma_insert(&new_vmas, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP, VMM_USER_READ | VMM_USER_WRITE, 0,
                  VMA_STACK)) {
    vmm_destroy(&new_as);
    vma_list_destroy(&new_vmas);
    return -(int64_t)EINVAL;
  }
  kprintf("[spore] execve %s\n", path);
  if (!cell_exec_replace(&new_as, &new_vmas, elf.runtime_entry, user_sp, frame, path, argv, argc)) {
    vmm_destroy(&new_as);
    vma_list_destroy(&new_vmas);
    return -12;
  }
  cell_apply_exec_creds(exec_node.mode, exec_node.uid, exec_node.gid);
  return 0;
}

