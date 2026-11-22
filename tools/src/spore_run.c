#define _GNU_SOURCE

#include "runner/runner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(void) {
  fputs("usage: spore-run [--mode plain|filter|shell|stdin|bench|rng] "
        "[--timings] --image IMAGE [--root ROOT_EXT2] [--qemu QEMU] [--accel ACCEL] [--cpu CPU] [--memory MEM] "
        "[--smp N] [--vars VARS_FD]\n",
        stderr);
  exit(2);
}

static bool ends_with(const char *s, const char *suffix) {
  size_t s_len = strlen(s);
  size_t suffix_len = strlen(suffix);
  return s_len >= suffix_len && strcmp(s + s_len - suffix_len, suffix) == 0;
}

static bool find_firmware(const char *qemu, char *out, size_t cap) {
  char cmd[PATH_CAP];
  snprintf(cmd, sizeof(cmd), "%s -L help 2>/dev/null", qemu);
  FILE *pipe = popen(cmd, "r");
  if (pipe != NULL) {
    char dir[PATH_CAP];
    while (fscanf(pipe, "%1023s", dir) == 1) {
      snprintf(out, cap, "%s/edk2-aarch64-code.fd", dir);
      if (access(out, R_OK) == 0) {
        pclose(pipe);
        return true;
      }
      char *suffix = strstr(dir, "-firmware");
      if (suffix != NULL) { *suffix = '\0'; }
      snprintf(out, cap, "%s/qemu/edk2-aarch64-code.fd", dir);
      if (access(out, R_OK) == 0) {
        pclose(pipe);
        return true;
      }
    }
    pclose(pipe);
  }
  snprintf(out, cap, "/opt/homebrew/opt/qemu/share/qemu/edk2-aarch64-code.fd");
  return access(out, R_OK) == 0;
}

static bool find_vars_template(const char *qemu, char *out, size_t cap) {
  char cmd[PATH_CAP];
  snprintf(cmd, sizeof(cmd), "%s -L help 2>/dev/null", qemu);
  FILE *pipe = popen(cmd, "r");
  if (pipe != NULL) {
    char dir[PATH_CAP];
    while (fscanf(pipe, "%1023s", dir) == 1) {
      char base[PATH_CAP];
      snprintf(base, sizeof(base), "%s", dir);
      char *suffix = strstr(base, "-firmware");
      if (suffix != NULL) { *suffix = '\0'; }
      const char *patterns[] = {
        "%s/edk2-arm-vars.fd",
        "%s/edk2-aarch64-vars.fd",
        "%s/qemu/edk2-arm-vars.fd",
        "%s/qemu/edk2-aarch64-vars.fd",
      };
      for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i) {
        snprintf(out, cap, patterns[i], i < 2 ? dir : base);
        if (access(out, R_OK) == 0) {
          pclose(pipe);
          return true;
        }
      }
    }
    pclose(pipe);
  }
  const char *fallbacks[] = {
    "/opt/homebrew/opt/qemu/share/qemu/edk2-arm-vars.fd",
    "/opt/homebrew/opt/qemu/share/qemu/edk2-aarch64-vars.fd",
  };
  for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i) {
    snprintf(out, cap, "%s", fallbacks[i]);
    if (access(out, R_OK) == 0) { return true; }
  }
  return false;
}

static void copy_file_or_die(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (in == NULL) {
    perror(src);
    exit(1);
  }
  FILE *out = fopen(dst, "wb");
  if (out == NULL) {
    perror(dst);
    fclose(in);
    exit(1);
  }
  char buf[65536];
  for (;;) {
    size_t n = fread(buf, 1, sizeof(buf), in);
    if (n > 0 && fwrite(buf, 1, n, out) != n) {
      perror(dst);
      exit(1);
    }
    if (n < sizeof(buf)) {
      if (ferror(in)) {
        perror(src);
        exit(1);
      }
      break;
    }
  }
  fclose(in);
  fclose(out);
}

static void ensure_vars_file(const char *qemu, const char *vars) {
  if (vars == NULL || access(vars, R_OK | W_OK) == 0) { return; }
  char tmpl[PATH_CAP];
  if (!find_vars_template(qemu, tmpl, sizeof(tmpl))) {
    fputs("spore-run: edk2 vars template not found\n", stderr);
    exit(1);
  }
  copy_file_or_die(tmpl, vars);
}

static void build_qemu_args(char **argv, int *argc, const char *qemu, const char *image, const char *root,
                            const char *accel, const char *cpu, const char *memory, const char *smp, const char *vars,
                            char *firmware_arg, size_t firmware_cap, char *vars_arg, size_t vars_cap,
                            char *image_drive_arg, size_t image_drive_cap, char *root_drive_arg,
                            size_t root_drive_cap) {
  char firmware[PATH_CAP];
  if (!find_firmware(qemu, firmware, sizeof(firmware))) {
    fputs("spore-run: edk2-aarch64-code.fd not found\n", stderr);
    exit(1);
  }
  ensure_vars_file(qemu, vars);
  snprintf(firmware_arg, firmware_cap, "if=pflash,format=raw,readonly=on,file=%s", firmware);
  if (vars != NULL) { snprintf(vars_arg, vars_cap, "if=pflash,format=raw,file=%s", vars); }
  snprintf(image_drive_arg, image_drive_cap, "if=none,format=raw,readonly=on,file=%s,id=sporeesp", image);
  if (root != NULL) { snprintf(root_drive_arg, root_drive_cap, "if=none,format=raw,file=%s,id=sporeroot", root); }
  int i = 0;
  argv[i++] = (char *)qemu;
  argv[i++] = "-M";
  argv[i++] = "virt,gic-version=3,virtualization=off";
  argv[i++] = "-accel";
  argv[i++] = (char *)accel;
  argv[i++] = "-cpu";
  argv[i++] = (char *)cpu;
  argv[i++] = "-m";
  argv[i++] = (char *)memory;
  argv[i++] = "-smp";
  argv[i++] = (char *)smp;
  argv[i++] = "-global";
  argv[i++] = "virtio-mmio.force-legacy=false";
  argv[i++] = "-netdev";
  argv[i++] = "user,id=sporenet";
  argv[i++] = "-device";
  argv[i++] = "virtio-net-device,netdev=sporenet,mac=52:54:00:12:34:56";
  argv[i++] = "-boot";
  argv[i++] = "order=d,menu=off,strict=on";
  argv[i++] = "-drive";
  argv[i++] = firmware_arg;
  if (vars != NULL) {
    argv[i++] = "-drive";
    argv[i++] = vars_arg;
  }
  if (ends_with(image, ".iso")) {
    argv[i++] = "-cdrom";
    argv[i++] = (char *)image;
  } else {
    argv[i++] = "-drive";
    argv[i++] = image_drive_arg;
    argv[i++] = "-device";
    argv[i++] = "virtio-blk-device,drive=sporeesp,bootindex=0";
  }
  if (root != NULL) {
    argv[i++] = "-drive";
    argv[i++] = root_drive_arg;
    argv[i++] = "-device";
    argv[i++] = "virtio-blk-device,drive=sporeroot";
  }
  argv[i++] = "-serial";
  argv[i++] = "stdio";
  argv[i++] = "-display";
  argv[i++] = "none";
  argv[i] = NULL;
  *argc = i;
}

int main(int argc, char **argv) {
  restore_host_terminal_mouse();
  atexit(restore_host_terminal_mouse);

  const char *mode = "filter";
  const char *image = NULL;
  const char *root = NULL;
  const char *qemu = "qemu-system-aarch64";
  const char *accel = "hvf";
  const char *cpu = "host";
  const char *memory = "8G";
  const char *smp = "4";
  const char *vars = NULL;
  bool timings = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      mode = argv[++i];
    } else if (strcmp(argv[i], "--timings") == 0) {
      timings = true;
    } else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
      image = argv[++i];
    } else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
      root = argv[++i];
    } else if (strcmp(argv[i], "--qemu") == 0 && i + 1 < argc) {
      qemu = argv[++i];
    } else if (strcmp(argv[i], "--accel") == 0 && i + 1 < argc) {
      accel = argv[++i];
    } else if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
      cpu = argv[++i];
    } else if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc) {
      memory = argv[++i];
    } else if (strcmp(argv[i], "--smp") == 0 && i + 1 < argc) {
      smp = argv[++i];
    } else if (strcmp(argv[i], "--vars") == 0 && i + 1 < argc) {
      vars = argv[++i];
    } else {
      usage();
    }
  }
  if (image == NULL) { usage(); }
  char firmware_arg[PATH_CAP];
  char vars_arg[PATH_CAP];
  char image_drive_arg[PATH_CAP];
  char root_drive_arg[PATH_CAP];
  char *qemu_argv[96];
  int qemu_argc = 0;
  build_qemu_args(qemu_argv, &qemu_argc, qemu, image, root, accel, cpu, memory, smp, vars, firmware_arg,
                  sizeof(firmware_arg), vars_arg, sizeof(vars_arg), image_drive_arg, sizeof(image_drive_arg),
                  root_drive_arg, sizeof(root_drive_arg));
  (void)qemu_argc;
  if (strcmp(mode, "plain") == 0 || strcmp(mode, "filter") == 0 || strcmp(mode, "shell") == 0 ||
      strcmp(mode, "stdin") == 0 || strcmp(mode, "bench") == 0 || strcmp(mode, "rng") == 0) {
    return run_harness(qemu_argv, mode, timings);
  }
  usage();
  return 2;
}
