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
  E2BIG = 7,
  EFAULT = 14,
  EACCES = 13,
  ENOMEM = 12,
  EINVAL = 22,
  ENOENT = 2,
  EPERM = 1,
  X_OK = 1,
  EXEC_ARG_MAX_BYTES = USER_STACK_SIZE / 16,
};

#define INTERP_LOAD_BASE 0x0000006000000000ull
#define EXEC_LOAD_BASE 0x0000004000000000ull

static bool streq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) { return false; }
  }
  return *a == '\0' && *b == '\0';
}

static bool copy_exec_path(uint64_t path_addr, char *out, size_t cap) {
  char raw[CELL_PATH_MAX];
  char virtual_path[CELL_PATH_MAX];
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

static void free_pages(uint64_t pa, uint64_t pages) {
  for (uint64_t i = 0; i < pages; ++i) {
    pmm_free_page(pa + i * PAGE_SIZE);
  }
}

struct exec_scratch {
  uint64_t ptr_pa;
  uint64_t ptr_pages;
  uint64_t data_pa;
  uint64_t data_pages;
  const char **argv;
  const char **envp;
  uint64_t argv_cap;
  uint64_t env_cap;
  char *data;
  uint64_t data_cap;
  uint64_t data_len;
};

static bool add_arg_budget(uint64_t *used, uint64_t amount) {
  if (amount > EXEC_ARG_MAX_BYTES || *used > EXEC_ARG_MAX_BYTES - amount) { return false; }
  *used += amount;
  return true;
}

static bool read_user_u64(uint64_t addr, uint64_t *out) {
  return syscall_user_readable(addr, sizeof(*out)) && vmm_copy_from_user(syscall_active_as(), out, addr, sizeof(*out));
}

static int measure_user_string(uint64_t ptr, uint64_t *budget_used, uint64_t *out_len) {
  uint64_t len = 0;
  for (;;) {
    if (!add_arg_budget(budget_used, 1)) { return -E2BIG; }
    char ch = 0;
    if (!vmm_copy_from_user(syscall_active_as(), &ch, ptr + len, 1)) { return -EFAULT; }
    ++len;
    if (ch == '\0') {
      *out_len = len;
      return 0;
    }
  }
}

static int measure_user_string_array(uint64_t user, uint64_t *out_count, uint64_t *out_string_bytes,
                                     uint64_t *budget_used) {
  *out_count = 0;
  *out_string_bytes = 0;
  if (user == 0) {
    if (!add_arg_budget(budget_used, sizeof(uint64_t))) { return -E2BIG; }
    return 0;
  }
  for (;;) {
    uint64_t ptr = 0;
    if (!read_user_u64(user + *out_count * sizeof(uint64_t), &ptr)) { return -EFAULT; }
    if (ptr == 0) {
      if (!add_arg_budget(budget_used, sizeof(uint64_t))) { return -E2BIG; }
      return 0;
    }
    if (!add_arg_budget(budget_used, sizeof(uint64_t))) { return -E2BIG; }
    uint64_t len = 0;
    int rc = measure_user_string(ptr, budget_used, &len);
    if (rc < 0) { return rc; }
    *out_string_bytes += len;
    ++*out_count;
  }
}

static bool exec_scratch_alloc(struct exec_scratch *scratch, uint64_t argv_cap, uint64_t env_cap,
                               uint64_t data_cap, uint64_t hhdm_offset) {
  kmemset(scratch, 0, sizeof(*scratch));
  scratch->argv_cap = argv_cap;
  scratch->env_cap = env_cap;
  scratch->data_cap = data_cap;

  uint64_t ptr_count = argv_cap + env_cap;
  if (ptr_count < argv_cap) { return false; }
  uint64_t ptr_bytes = ptr_count * sizeof(char *);
  if (ptr_count != 0 && ptr_bytes / sizeof(char *) != ptr_count) { return false; }
  scratch->ptr_pages = ptr_bytes == 0 ? 0 : (ptr_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  if (scratch->ptr_pages != 0) {
    scratch->ptr_pa = pmm_alloc_contiguous_pages(scratch->ptr_pages);
    if (scratch->ptr_pa == 0) { return false; }
    scratch->argv = (const char **)(uintptr_t)(hhdm_offset + scratch->ptr_pa);
    scratch->envp = scratch->argv + argv_cap;
    kmemset((void *)scratch->argv, 0, scratch->ptr_pages * PAGE_SIZE);
  }

  scratch->data_pages = data_cap == 0 ? 0 : (data_cap + PAGE_SIZE - 1) / PAGE_SIZE;
  if (scratch->data_pages != 0) {
    scratch->data_pa = pmm_alloc_contiguous_pages(scratch->data_pages);
    if (scratch->data_pa == 0) {
      if (scratch->ptr_pa != 0) { free_pages(scratch->ptr_pa, scratch->ptr_pages); }
      kmemset(scratch, 0, sizeof(*scratch));
      return false;
    }
    scratch->data = (char *)(uintptr_t)(hhdm_offset + scratch->data_pa);
  }
  return true;
}

static void exec_scratch_free(struct exec_scratch *scratch) {
  if (scratch->ptr_pa != 0) { free_pages(scratch->ptr_pa, scratch->ptr_pages); }
  if (scratch->data_pa != 0) { free_pages(scratch->data_pa, scratch->data_pages); }
  kmemset(scratch, 0, sizeof(*scratch));
}

static int scratch_add_bytes(struct exec_scratch *scratch, const void *src, uint64_t len, const char **out) {
  if (len == 0 || len > scratch->data_cap || scratch->data_len > scratch->data_cap - len) { return -E2BIG; }
  char *dst = scratch->data + scratch->data_len;
  kmemcpy(dst, src, len);
  scratch->data_len += len;
  *out = dst;
  return 0;
}

static int scratch_add_kernel_string(struct exec_scratch *scratch, const char *src, const char **out) {
  return scratch_add_bytes(scratch, src, kstrlen(src) + 1, out);
}

static int scratch_add_user_string(struct exec_scratch *scratch, uint64_t ptr, const char **out) {
  uint64_t start = scratch->data_len;
  for (;;) {
    if (scratch->data_len >= scratch->data_cap) { return -E2BIG; }
    char ch = 0;
    if (!vmm_copy_from_user(syscall_active_as(), &ch, ptr + scratch->data_len - start, 1)) { return -EFAULT; }
    scratch->data[scratch->data_len++] = ch;
    if (ch == '\0') {
      *out = scratch->data + start;
      return 0;
    }
  }
}

static int copy_user_string_array(struct exec_scratch *scratch, uint64_t user, const char **out, uint64_t count) {
  if (user == 0) { return count == 0 ? 0 : -EFAULT; }
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t ptr = 0;
    if (!read_user_u64(user + i * sizeof(uint64_t), &ptr) || ptr == 0) { return -EFAULT; }
    int rc = scratch_add_user_string(scratch, ptr, &out[i]);
    if (rc < 0) { return rc; }
  }
  return 0;
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
  char path[CELL_PATH_MAX];
  if (!copy_exec_path(path_addr, path, sizeof(path))) { return -(int64_t)EFAULT; }
  if (!cell_fs_path_allowed(path, CELL_FS_EXEC)) { return -(int64_t)EPERM; }

  uint64_t argc = 0;
  uint64_t envc = 0;
  uint64_t argv_string_bytes = 0;
  uint64_t env_string_bytes = 0;
  uint64_t budget_used = sizeof(uint64_t);
  int rc = measure_user_string_array(argv_addr, &argc, &argv_string_bytes, &budget_used);
  if (rc < 0) { return rc; }
  rc = measure_user_string_array(envp_addr, &envc, &env_string_bytes, &budget_used);
  if (rc < 0) { return rc; }
  if (argc == 0 &&
      (!add_arg_budget(&budget_used, sizeof(uint64_t)) || !add_arg_budget(&budget_used, kstrlen(path) + 1))) {
    return -(int64_t)E2BIG;
  }

  uint64_t argv_cap = (argc == 0 ? 1 : argc) + 3;
  if (argv_cap < argc) { return -(int64_t)E2BIG; }
  uint64_t data_cap = argv_string_bytes + env_string_bytes + CELL_PATH_MAX + 256;
  if (argc == 0) { data_cap += kstrlen(path) + 1; }
  if (data_cap < argv_string_bytes || data_cap < env_string_bytes) { return -(int64_t)E2BIG; }

  struct exec_scratch scratch;
  uint64_t hhdm_offset = syscall_active_as()->hhdm_offset;
  if (!exec_scratch_alloc(&scratch, argv_cap, envc, data_cap, hhdm_offset)) { return -(int64_t)ENOMEM; }
  const char **argv = scratch.argv;
  const char **envp = scratch.envp;

  int64_t ret = 0;
  rc = copy_user_string_array(&scratch, argv_addr, argv, argc);
  if (rc < 0) {
    ret = rc;
    goto out_scratch;
  }
  if (argc == 0) {
    rc = scratch_add_kernel_string(&scratch, path, &argv[0]);
    if (rc < 0) {
      ret = rc;
      goto out_scratch;
    }
    argc = 1;
  }
  rc = copy_user_string_array(&scratch, envp_addr, envp, envc);
  if (rc < 0) {
    ret = rc;
    goto out_scratch;
  }

  struct vfs_node exec_node;
  if (!vfs_lookup(path, &exec_node)) {
    ret = -(int64_t)ENOENT;
    goto out_scratch;
  }
  if (!syscall_node_access_allowed(&exec_node, X_OK)) {
    ret = -(int64_t)EACCES;
    goto out_scratch;
  }
  struct elf_reader exec_reader;
  if (!make_elf_reader(&exec_node, &exec_reader)) {
    ret = -(int64_t)ENOENT;
    goto out_scratch;
  }
  char shebang_interp[128];
  char shebang_arg[128];
  bool shebang_has_arg = false;
  if (parse_shebang_node(&exec_node, shebang_interp, sizeof(shebang_interp), shebang_arg, sizeof(shebang_arg),
                         &shebang_has_arg)) {
    struct vfs_node shebang_node;
    if (!vfs_lookup(shebang_interp, &shebang_node) || !make_elf_reader(&shebang_node, &exec_reader)) {
      ret = -(int64_t)ENOENT;
      goto out_scratch;
    }
    uint64_t old_argc = argc;
    uint64_t extra = 2 + (shebang_has_arg ? 1 : 0);
    uint64_t trailing = old_argc > 0 ? old_argc - 1 : 0;
    uint64_t new_argc = extra + trailing;
    if (new_argc > scratch.argv_cap) {
      ret = -(int64_t)E2BIG;
      goto out_scratch;
    }
    for (uint64_t i = trailing; i > 0; --i) {
      argv[extra + i - 1] = argv[i];
    }
    uint64_t idx = 0;
    rc = scratch_add_kernel_string(&scratch, shebang_interp, &argv[idx++]);
    if (rc < 0) {
      ret = rc;
      goto out_scratch;
    }
    if (shebang_has_arg) {
      rc = scratch_add_kernel_string(&scratch, shebang_arg, &argv[idx++]);
      if (rc < 0) {
        ret = rc;
        goto out_scratch;
      }
    }
    rc = scratch_add_kernel_string(&scratch, path, &argv[idx++]);
    if (rc < 0) {
      ret = rc;
      goto out_scratch;
    }
    argc = new_argc;
    exec_node = shebang_node;
  }
  char interp_path[128];
  bool has_interp = elf_find_interp_aarch64(&exec_reader, interp_path, sizeof(interp_path));

  struct user_address_space new_as;
  struct loaded_elf elf;
  struct vma_list new_vmas;
  uint64_t user_sp = 0;
  vma_list_init(&new_vmas);
  if (!vmm_user_init(&new_as, hhdm_offset)) {
    ret = -(int64_t)ENOMEM;
    goto out_scratch;
  }
  if (!elf_load_aarch64(&new_as, &new_vmas, &exec_reader, EXEC_LOAD_BASE, &elf)) {
    vmm_destroy(&new_as);
    vma_list_destroy(&new_vmas);
    ret = -(int64_t)EINVAL;
    goto out_scratch;
  }
  if (has_interp) {
    struct vfs_node interp_node;
    struct elf_reader interp_reader;
    struct loaded_elf interp;
    if (!vfs_lookup(interp_path, &interp_node) || !make_elf_reader(&interp_node, &interp_reader) ||
        !elf_load_aarch64(&new_as, &new_vmas, &interp_reader, INTERP_LOAD_BASE, &interp)) {
      vmm_destroy(&new_as);
      vma_list_destroy(&new_vmas);
      ret = -(int64_t)ENOENT;
      goto out_scratch;
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
    ret = -(int64_t)EINVAL;
    goto out_scratch;
  }
  if (!vma_insert(&new_vmas, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP, VMM_USER_READ | VMM_USER_WRITE, 0,
                  VMA_STACK)) {
    vmm_destroy(&new_as);
    vma_list_destroy(&new_vmas);
    ret = -(int64_t)EINVAL;
    goto out_scratch;
  }
  kprintf("[spore] execve %s\n", path);
  if (!cell_exec_replace(&new_as, &new_vmas, elf.runtime_entry, user_sp, frame, path, argv, argc)) {
    vmm_destroy(&new_as);
    vma_list_destroy(&new_vmas);
    ret = -(int64_t)ENOMEM;
    goto out_scratch;
  }
  cell_apply_exec_creds(exec_node.mode, exec_node.uid, exec_node.gid);
  ret = 0;

out_scratch:
  exec_scratch_free(&scratch);
  return ret;
}
