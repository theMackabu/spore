#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
  MAX_PATH = 1024,
  MAX_LINE = 512,
};

static void die(const char *msg) {
  perror(msg);
  exit(1);
}

static void die_msg(const char *msg) {
  fputs(msg, stderr);
  fputc('\n', stderr);
  exit(1);
}

static bool exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static bool is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static uint64_t file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) { die(path); }
  return (uint64_t)st.st_size;
}

static uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static void path_join(char *out, size_t cap, const char *a, const char *b) {
  int n = snprintf(out, cap, "%s/%s", a, b);
  if (n < 0 || (size_t)n >= cap) { die_msg("path too long"); }
}

static void ensure_dir(const char *path) {
  char tmp[MAX_PATH];
  size_t len = strlen(path);
  if (len >= sizeof(tmp)) { die_msg("path too long"); }
  memcpy(tmp, path, len + 1);
  for (char *p = tmp + 1; *p != '\0'; ++p) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { die("mkdir"); }
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { die("mkdir"); }
}

static void remove_tree(const char *path) {
  if (!exists(path)) { return; }
  char cmd[MAX_PATH + 32];
  int n = snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
  if (n < 0 || (size_t)n >= sizeof(cmd)) { die_msg("command too long"); }
  if (system(cmd) != 0) { die_msg("rm -rf failed"); }
}

static void copy_file(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (in == NULL) { die(src); }
  FILE *out = fopen(dst, "wb");
  if (out == NULL) { die(dst); }
  char buf[65536];
  for (;;) {
    size_t n = fread(buf, 1, sizeof(buf), in);
    if (n > 0 && fwrite(buf, 1, n, out) != n) { die("fwrite"); }
    if (n < sizeof(buf)) {
      if (ferror(in)) { die("fread"); }
      break;
    }
  }
  fclose(in);
  fclose(out);
}

static void write_text_file(const char *path, const char *text) {
  FILE *out = fopen(path, "wb");
  if (out == NULL) { die(path); }
  if (fputs(text, out) == EOF) { die("fputs"); }
  fclose(out);
}

static void create_sparse_file(const char *path, uint64_t bytes) {
  FILE *out = fopen(path, "wb");
  if (out == NULL) { die(path); }
  if (ftruncate(fileno(out), (off_t)bytes) != 0) { die("ftruncate"); }
  fclose(out);
}

static void run_argv(char *const argv[], bool quiet) {
  pid_t pid = fork();
  if (pid < 0) { die("fork"); }
  if (pid == 0) {
    if (quiet) {
      FILE *devnull = fopen("/dev/null", "w");
      if (devnull != NULL) {
        dup2(fileno(devnull), STDOUT_FILENO);
        dup2(fileno(devnull), STDERR_FILENO);
      }
    }
    execvp(argv[0], argv);
    perror(argv[0]);
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) { die("waitpid"); }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { die_msg("command failed"); }
}

static void run_debugfs_script(const char *image, const char *script) {
  char script_path[MAX_PATH];
  int n = snprintf(script_path, sizeof(script_path), "%s.debugfs", image);
  if (n < 0 || (size_t)n >= sizeof(script_path)) { die_msg("path too long"); }
  write_text_file(script_path, script);
  char *const argv[] = {"debugfs", "-w", "-f", script_path, (char *)image, NULL};
  run_argv(argv, true);
  unlink(script_path);
}

static const char *basename_of(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash == NULL ? path : slash + 1;
}

static void stem_of(char *out, size_t cap, const char *path) {
  const char *base = basename_of(path);
  snprintf(out, cap, "%s", base);
  char *dot = strrchr(out, '.');
  if (dot != NULL) { *dot = '\0'; }
}

static void module_name(char *out, size_t cap, const char *target) {
  size_t pos = 0;
  for (const char *p = target; *p != '\0'; ++p) {
    if (*p == '/' && pos == 0) { continue; }
    if (pos + 1 >= cap) { die_msg("module name too long"); }
    out[pos++] = *p == '/' ? '_' : *p;
    if (*p == '/' && pos + 1 < cap) { out[pos++] = '_'; }
  }
  if (pos == 0) {
    snprintf(out, cap, "init");
  } else {
    out[pos] = '\0';
  }
}

static bool prebuilt_path(char *out, size_t cap, const char *prebuilt_root, const char *source_root,
                          const char *source) {
  if (prebuilt_root == NULL || prebuilt_root[0] == '\0') { return false; }
  int exact = snprintf(out, cap, "%s/%s", prebuilt_root, source);
  if (exact < 0 || (size_t)exact >= cap) { die_msg("path too long"); }
  if (exists(out) && !is_dir(out)) { return true; }

  char src[MAX_PATH];
  path_join(src, sizeof(src), source_root, source);
  char name[MAX_PATH];
  if (is_dir(src)) {
    snprintf(name, sizeof(name), "%s", basename_of(src));
  } else {
    stem_of(name, sizeof(name), src);
  }
  int n = snprintf(out, cap, "%s/%s/%s", prebuilt_root, source, name);
  if (n < 0 || (size_t)n >= cap) { die_msg("path too long"); }
  return exists(out);
}

static void compile_or_copy_userland(const char *source_root, const char *source, const char *out,
                                     const char *prebuilt_root) {
  char prebuilt[MAX_PATH];
  if (prebuilt_path(prebuilt, sizeof(prebuilt), prebuilt_root, source_root, source)) {
    copy_file(prebuilt, out);
    return;
  }

  char src[MAX_PATH];
  path_join(src, sizeof(src), source_root, source);
  char include[MAX_PATH];
  snprintf(include, sizeof(include), "-I%s/userland/lib/spore", source_root);

  if (is_dir(src)) {
    char util_c[MAX_PATH];
    snprintf(util_c, sizeof(util_c), "%s/userland/lib/spore/util.c", source_root);

    char source_files[32][MAX_PATH];
    size_t source_count = 0;
    DIR *dir = opendir(src);
    if (dir == NULL) { die(src); }
    for (struct dirent *entry = readdir(dir); entry != NULL; entry = readdir(dir)) {
      size_t len = strlen(entry->d_name);
      if (len > 2 && strcmp(entry->d_name + len - 2, ".c") == 0) {
        if (source_count >= sizeof(source_files) / sizeof(source_files[0])) { die_msg("too many userland sources"); }
        path_join(source_files[source_count++], sizeof(source_files[0]), src, entry->d_name);
      }
    }
    closedir(dir);
    if (source_count == 0) { die_msg("userland directory has no C sources"); }
    for (size_t i = 0; i + 1 < source_count; ++i) {
      for (size_t j = i + 1; j < source_count; ++j) {
        if (strcmp(source_files[i], source_files[j]) > 0) {
          char tmp[MAX_PATH];
          snprintf(tmp, sizeof(tmp), "%s", source_files[i]);
          snprintf(source_files[i], sizeof(source_files[i]), "%s", source_files[j]);
          snprintf(source_files[j], sizeof(source_files[j]), "%s", tmp);
        }
      }
    }

    char *argv[48];
    int argc = 0;
    argv[argc++] = "aarch64-unknown-linux-musl-gcc";
    bool static_binary = strcmp(source, "userland/bin/init") == 0;
    if (static_binary) { argv[argc++] = "-static"; }
    argv[argc++] = "-std=c23";
    argv[argc++] = "-D_GNU_SOURCE";
    argv[argc++] = "-s";
    if (!static_binary) {
      argv[argc++] = "-Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1";
      argv[argc++] = "-Wl,-rpath,/lib";
    }
    argv[argc++] = include;
    for (size_t i = 0; i < source_count; ++i) {
      argv[argc++] = source_files[i];
    }
    argv[argc++] = util_c;
    argv[argc++] = "-o";
    argv[argc++] = (char *)out;
    argv[argc] = NULL;
    run_argv(argv, false);
  } else if (strstr(src, ".c") == src + strlen(src) - 2) {
    bool static_binary = strcmp(source, "userland/bin/init") == 0;
    char *const argv[] = {
      "aarch64-unknown-linux-musl-gcc",
      static_binary ? "-static" : "-Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1",
      "-std=c23",
      "-D_GNU_SOURCE",
      "-s",
      "-Wl,-rpath,/lib",
      src,
      "-o",
      (char *)out,
      NULL,
    };
    run_argv(argv, false);
  } else {
    copy_file(src, out);
  }
}

static void mcopy_into(const char *efi_img, const char *src, const char *dst) {
  char target[MAX_PATH];
  snprintf(target, sizeof(target), "::%s", dst);
  char *const argv[] = {"mcopy", "-i", (char *)efi_img, (char *)src, target, NULL};
  run_argv(argv, false);
}

static void copy_into_rootfs(const char *src, const char *rootfs, const char *dst) {
  char out[MAX_PATH];
  int n = snprintf(out, sizeof(out), "%s%s", rootfs, dst);
  if (n < 0 || (size_t)n >= sizeof(out)) { die_msg("path too long"); }

  char parent[MAX_PATH];
  snprintf(parent, sizeof(parent), "%s", out);
  char *slash = strrchr(parent, '/');
  if (slash != NULL) {
    *slash = '\0';
    ensure_dir(parent);
  }
  copy_file(src, out);
  bool executable = strcmp(dst, "/init") == 0 || strncmp(dst, "/bin/", 5) == 0 || strncmp(dst, "/usr/bin/", 9) == 0 ||
                    strncmp(dst, "/usr/local/bin/", 15) == 0 || strncmp(dst, "/demos/", 7) == 0;
  chmod(out, executable ? 0755 : 0644);
}

static void symlink_into_rootfs(const char *target, const char *rootfs, const char *dst) {
  char out[MAX_PATH];
  int n = snprintf(out, sizeof(out), "%s%s", rootfs, dst);
  if (n < 0 || (size_t)n >= sizeof(out)) { die_msg("path too long"); }

  char parent[MAX_PATH];
  snprintf(parent, sizeof(parent), "%s", out);
  char *slash = strrchr(parent, '/');
  if (slash != NULL) {
    *slash = '\0';
    ensure_dir(parent);
  }
  unlink(out);
  if (symlink(target, out) != 0) { die("symlink"); }
}

static void write_rootfs_text(const char *rootfs, const char *path, const char *text, mode_t mode) {
  char out[MAX_PATH];
  int n = snprintf(out, sizeof(out), "%s%s", rootfs, path);
  if (n < 0 || (size_t)n >= sizeof(out)) { die_msg("path too long"); }
  char parent[MAX_PATH];
  snprintf(parent, sizeof(parent), "%s", out);
  char *slash = strrchr(parent, '/');
  if (slash != NULL) {
    *slash = '\0';
    ensure_dir(parent);
  }
  write_text_file(out, text);
  chmod(out, mode);
}

static void install_default_accounts(const char *rootfs) {
  char root_home[MAX_PATH];
  char spore_home[MAX_PATH];
  path_join(root_home, sizeof(root_home), rootfs, "root");
  path_join(spore_home, sizeof(spore_home), rootfs, "home/spore");
  ensure_dir(root_home);
  ensure_dir(spore_home);

  write_rootfs_text(rootfs, "/etc/passwd",
                    "root:x:0:0:root:/root:/bin/msh\n"
                    "spore:x:1000:1000:spore:/home/spore:/bin/msh\n",
                    0644);
  write_rootfs_text(rootfs, "/etc/group",
                    "root:x:0:\n"
                    "spore:x:1000:\n"
                    "sudo:x:27:spore\n",
                    0644);
  write_rootfs_text(rootfs, "/etc/shadow",
                    "root:!:0:0:99999:7:::\n"
                    "spore::0:0:99999:7:::\n",
                    0600);
  write_rootfs_text(rootfs, "/etc/sudoers",
                    "root ALL=(ALL) ALL\n"
                    "spore ALL=(ALL) NOPASSWD: ALL\n",
                    0440);
  write_rootfs_text(rootfs, "/root/.profile", "# root login profile for msh\n", 0644);
  write_rootfs_text(rootfs, "/root/.mshrc", "# interactive root msh startup\n", 0644);
  write_rootfs_text(rootfs, "/home/spore/.profile", "# spore login profile for msh\n", 0644);
  write_rootfs_text(rootfs, "/home/spore/.mshrc", "# interactive spore msh startup\n", 0644);
}

static void install_musl_runtime(const char *source_root, const char *rootfs) {
  (void)source_root;
  char lib_dir[MAX_PATH];
  path_join(lib_dir, sizeof(lib_dir), rootfs, "lib");
  ensure_dir(lib_dir);

  char ld_src[MAX_PATH];
  char libc_src[MAX_PATH];
  FILE *ld_pipe = popen("aarch64-unknown-linux-musl-gcc -print-file-name=ld-musl-aarch64.so.1", "r");
  if (ld_pipe == NULL || fgets(ld_src, sizeof(ld_src), ld_pipe) == NULL || pclose(ld_pipe) != 0) {
    die_msg("failed to locate ld-musl-aarch64.so.1");
  }
  FILE *libc_pipe = popen("aarch64-unknown-linux-musl-gcc -print-file-name=libc.so", "r");
  if (libc_pipe == NULL || fgets(libc_src, sizeof(libc_src), libc_pipe) == NULL || pclose(libc_pipe) != 0) {
    die_msg("failed to locate libc.so");
  }
  ld_src[strcspn(ld_src, "\r\n")] = '\0';
  libc_src[strcspn(libc_src, "\r\n")] = '\0';

  char ld_dst[MAX_PATH];
  path_join(ld_dst, sizeof(ld_dst), lib_dir, "ld-musl-aarch64.so.1");
  copy_file(ld_src, ld_dst);
  chmod(ld_dst, 0755);

  char libc_dst[MAX_PATH];
  path_join(libc_dst, sizeof(libc_dst), lib_dir, "libc.so");
  copy_file(libc_src, libc_dst);
  chmod(libc_dst, 0755);
}

static void build_root_ext2(const char *rootfs_dir, const char *output_root, const char *output_copy) {
  char dev_dir[MAX_PATH];
  char etc_dir[MAX_PATH];
  char proc_dir[MAX_PATH];
  char tmp_dir[MAX_PATH];
  path_join(dev_dir, sizeof(dev_dir), rootfs_dir, "dev");
  path_join(etc_dir, sizeof(etc_dir), rootfs_dir, "etc");
  path_join(proc_dir, sizeof(proc_dir), rootfs_dir, "proc");
  path_join(tmp_dir, sizeof(tmp_dir), rootfs_dir, "tmp");
  ensure_dir(dev_dir);
  ensure_dir(etc_dir);
  ensure_dir(proc_dir);
  ensure_dir(tmp_dir);

  char *const mkfs_argv[] = {
    "mke2fs", "-q", "-t", "ext2", "-b", "4096", "-d", (char *)rootfs_dir, (char *)output_root, "131072", NULL,
  };
  unlink(output_root);
  run_argv(mkfs_argv, false);

  run_debugfs_script(output_root, "set_inode_field /home/spore uid 1000\n"
                                  "set_inode_field /home/spore gid 1000\n"
                                  "set_inode_field /home/spore/.profile uid 1000\n"
                                  "set_inode_field /home/spore/.profile gid 1000\n"
                                  "set_inode_field /home/spore/.mshrc uid 1000\n"
                                  "set_inode_field /home/spore/.mshrc gid 1000\n");
  char sudo_path[MAX_PATH];
  path_join(sudo_path, sizeof(sudo_path), rootfs_dir, "bin/sudo");
  if (exists(sudo_path)) {
    run_debugfs_script(output_root, "set_inode_field /bin/sudo uid 0\n"
                                    "set_inode_field /bin/sudo gid 0\n"
                                    "set_inode_field /bin/sudo mode 0104755\n");
  }
  char su_path[MAX_PATH];
  path_join(su_path, sizeof(su_path), rootfs_dir, "bin/su");
  if (exists(su_path)) {
    run_debugfs_script(output_root, "set_inode_field /bin/su uid 0\n"
                                    "set_inode_field /bin/su gid 0\n"
                                    "set_inode_field /bin/su mode 0104755\n");
  }
  char passwd_path[MAX_PATH];
  path_join(passwd_path, sizeof(passwd_path), rootfs_dir, "bin/passwd");
  if (exists(passwd_path)) {
    run_debugfs_script(output_root, "set_inode_field /bin/passwd uid 0\n"
                                    "set_inode_field /bin/passwd gid 0\n"
                                    "set_inode_field /bin/passwd mode 0104755\n");
  }
  run_debugfs_script(output_root, "set_inode_field /etc/shadow uid 0\n"
                                  "set_inode_field /etc/shadow gid 0\n"
                                  "set_inode_field /etc/sudoers uid 0\n"
                                  "set_inode_field /etc/sudoers gid 0\n");
  copy_file(output_root, output_copy);
}

int main(int argc, char **argv) {
  if (argc != 9 && argc != 10 && argc != 11) {
    fputs("usage: spore-image SOURCE_ROOT ISO_ROOT KERNEL_ELF BOOT_EFI OUTPUT_IMAGE OUTPUT_COPY OUTPUT_ROOT_EXT2 "
          "OUTPUT_ROOT_COPY [MANIFEST] [PREBUILT_ROOT]\n",
          stderr);
    return 2;
  }
  const char *source_root = argv[1];
  const char *iso_root = argv[2];
  const char *kernel_elf = argv[3];
  const char *boot_efi = argv[4];
  const char *output_image = argv[5];
  const char *output_copy = argv[6];
  const char *output_root = argv[7];
  const char *output_root_copy = argv[8];
  char default_manifest[MAX_PATH];
  snprintf(default_manifest, sizeof(default_manifest), "%s/userland/image.manifest", source_root);
  const char *manifest = argc >= 10 ? argv[9] : default_manifest;
  const char *prebuilt_root = argc == 11 ? argv[10] : NULL;

  remove_tree(iso_root);
  char programs_dir[MAX_PATH];
  char modules_dir[MAX_PATH];
  char efi_boot_dir[MAX_PATH];
  char rootfs_dir[MAX_PATH];
  path_join(programs_dir, sizeof(programs_dir), iso_root, "programs");
  path_join(modules_dir, sizeof(modules_dir), iso_root, "boot/modules");
  path_join(efi_boot_dir, sizeof(efi_boot_dir), iso_root, "EFI/BOOT");
  path_join(rootfs_dir, sizeof(rootfs_dir), iso_root, "rootfs");
  ensure_dir(programs_dir);
  ensure_dir(modules_dir);
  ensure_dir(efi_boot_dir);
  ensure_dir(rootfs_dir);

  char kernel_dst[MAX_PATH];
  char boot_dst[MAX_PATH];
  path_join(kernel_dst, sizeof(kernel_dst), iso_root, "boot/kernel.elf");
  path_join(boot_dst, sizeof(boot_dst), efi_boot_dir, "BOOTAA64.EFI");
  copy_file(kernel_elf, kernel_dst);
  copy_file(boot_efi, boot_dst);

  char modules_txt[MAX_PATH];
  path_join(modules_txt, sizeof(modules_txt), iso_root, "boot/modules.txt");
  FILE *mods = fopen(modules_txt, "w");
  if (mods == NULL) { die(modules_txt); }
  fprintf(mods, "# esp-path baked-path\n");

  FILE *mf = fopen(manifest, "r");
  if (mf == NULL) { die(manifest); }
  char line[MAX_LINE];
  while (fgets(line, sizeof(line), mf) != NULL) {
    char *p = line;
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    if (*p == '\0' || *p == '\n' || *p == '#') { continue; }
    char source[MAX_PATH];
    char target[MAX_PATH];
    if (sscanf(p, "%1023s %1023s", source, target) != 2) { die_msg("bad manifest line"); }
    if (strcmp(source, "dir") == 0) {
      char dir_path[MAX_PATH];
      int n = snprintf(dir_path, sizeof(dir_path), "%s%s", rootfs_dir, target);
      if (n < 0 || (size_t)n >= sizeof(dir_path)) { die_msg("path too long"); }
      ensure_dir(dir_path);
      continue;
    }
    if (strcmp(source, "symlink") == 0) {
      char link_path[MAX_PATH];
      if (sscanf(p, "%1023s %1023s %1023s", source, target, link_path) != 3) { die_msg("bad symlink line"); }
      symlink_into_rootfs(target, rootfs_dir, link_path);
      continue;
    }
    char name[MAX_PATH];
    module_name(name, sizeof(name), target);
    char out[MAX_PATH];
    path_join(out, sizeof(out), programs_dir, name);
    compile_or_copy_userland(source_root, source, out, prebuilt_root);
    copy_into_rootfs(out, rootfs_dir, target);
  }
  fclose(mf);
  fclose(mods);
  install_musl_runtime(source_root, rootfs_dir);
  install_default_accounts(rootfs_dir);

  uint64_t esp_payload = file_size(boot_efi) + file_size(kernel_elf) + file_size(modules_txt);
  uint64_t esp_size = align_up_u64(esp_payload * 2u + 2u * 1024u * 1024u, 1024u * 1024u);
  if (esp_size < 9u * 1024u * 1024u) { esp_size = 9u * 1024u * 1024u; }
  create_sparse_file(output_image, esp_size);
  char *const mkfs_argv[] = {"mkfs.fat", "-F", "16", (char *)output_image, NULL};
  run_argv(mkfs_argv, true);
  char *const mmd_argv[] = {"mmd",         "-i",      (char *)output_image, "::/EFI",
                            "::/EFI/BOOT", "::/boot", "::/boot/modules",    NULL};
  run_argv(mmd_argv, false);
  mcopy_into(output_image, boot_efi, "/EFI/BOOT/BOOTAA64.EFI");
  mcopy_into(output_image, kernel_elf, "/boot/kernel.elf");
  mcopy_into(output_image, modules_txt, "/boot/modules.txt");
  copy_file(output_image, output_copy);
  build_root_ext2(rootfs_dir, output_root, output_root_copy);
  return 0;
}
