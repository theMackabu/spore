#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
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
    argv[argc++] = "zig";
    argv[argc++] = "cc";
    argv[argc++] = "-target";
    argv[argc++] = "aarch64-linux-musl";
    argv[argc++] = "-static";
    argv[argc++] = "-std=c23";
    argv[argc++] = "-D_GNU_SOURCE";
    argv[argc++] = include;
    for (size_t i = 0; i < source_count; ++i) {
      argv[argc++] = source_files[i];
    }
    argv[argc++] = util_c;
    argv[argc++] = "-o";
    argv[argc++] = (char *)out;
    argv[argc] = NULL;
    run_argv(argv, false);
  } else {
    char *const argv[] = {
      "zig",           "cc", "-target", "aarch64-linux-musl", "-static", "-std=c23",
      "-D_GNU_SOURCE", src,  "-o",      (char *)out,          NULL,
    };
    run_argv(argv, false);
  }
}

static void mcopy_into(const char *efi_img, const char *src, const char *dst) {
  char target[MAX_PATH];
  snprintf(target, sizeof(target), "::%s", dst);
  char *const argv[] = {"mcopy", "-i", (char *)efi_img, (char *)src, target, NULL};
  run_argv(argv, false);
}

int main(int argc, char **argv) {
  if (argc != 7 && argc != 8 && argc != 9) {
    fputs("usage: spore-image SOURCE_ROOT ISO_ROOT KERNEL_ELF BOOT_EFI OUTPUT_IMAGE OUTPUT_COPY [MANIFEST] "
          "[PREBUILT_ROOT]\n",
          stderr);
    return 2;
  }
  const char *source_root = argv[1];
  const char *iso_root = argv[2];
  const char *kernel_elf = argv[3];
  const char *boot_efi = argv[4];
  const char *output_image = argv[5];
  const char *output_copy = argv[6];
  char default_manifest[MAX_PATH];
  snprintf(default_manifest, sizeof(default_manifest), "%s/userland/image.manifest", source_root);
  const char *manifest = argc >= 8 ? argv[7] : default_manifest;
  const char *prebuilt_root = argc == 9 ? argv[8] : NULL;

  remove_tree(iso_root);
  char modules_dir[MAX_PATH];
  char efi_boot_dir[MAX_PATH];
  path_join(modules_dir, sizeof(modules_dir), iso_root, "boot/modules");
  path_join(efi_boot_dir, sizeof(efi_boot_dir), iso_root, "EFI/BOOT");
  ensure_dir(modules_dir);
  ensure_dir(efi_boot_dir);

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
    char name[MAX_PATH];
    module_name(name, sizeof(name), target);
    char out[MAX_PATH];
    path_join(out, sizeof(out), modules_dir, name);
    compile_or_copy_userland(source_root, source, out, prebuilt_root);
    fprintf(mods, "boot/modules/%s %s\n", name, target);
  }
  fclose(mf);
  fclose(mods);

  char of_arg[MAX_PATH + 4];
  snprintf(of_arg, sizeof(of_arg), "of=%s", output_image);
  char *const dd_argv[] = {"dd", "if=/dev/zero", of_arg, "bs=1M", "count=64", NULL};
  run_argv(dd_argv, true);
  char *const mkfs_argv[] = {"mkfs.fat", "-F", "32", (char *)output_image, NULL};
  run_argv(mkfs_argv, true);
  char *const mmd_argv[] = {"mmd",         "-i",      (char *)output_image, "::/EFI",
                            "::/EFI/BOOT", "::/boot", "::/boot/modules",    NULL};
  run_argv(mmd_argv, false);
  mcopy_into(output_image, boot_efi, "/EFI/BOOT/BOOTAA64.EFI");
  mcopy_into(output_image, kernel_elf, "/boot/kernel.elf");
  mcopy_into(output_image, modules_txt, "/boot/modules.txt");

  char cmd[MAX_PATH * 3];
  snprintf(cmd, sizeof(cmd),
           "for f in '%s'/*; do mcopy -i '%s' \"$f\" ::/boot/modules/$(basename \"$f\") || exit 1; done", modules_dir,
           output_image);
  if (system(cmd) != 0) { die_msg("copy modules failed"); }
  copy_file(output_image, output_copy);
  return 0;
}
