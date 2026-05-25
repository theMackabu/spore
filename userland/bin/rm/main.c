#include <stdlib.h>

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct rm_options {
  bool recursive;
  bool force;
};

static bool streq_local(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}

static bool path_join(char *out, size_t cap, const char *dir, const char *name) {
  int n = snprintf(out, cap, "%s/%s", dir, name);
  return n >= 0 && (size_t)n < cap;
}

static int remove_path(const char *path, const struct rm_options *opts) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    if (opts->force && errno == ENOENT) { return EXIT_SUCCESS; }
    perror(path);
    return EXIT_FAILURE;
  }

  if (!S_ISDIR(st.st_mode)) {
    if (unlink(path) != 0) {
      if (opts->force && errno == ENOENT) { return EXIT_SUCCESS; }
      perror(path);
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }

  if (!opts->recursive) {
    fprintf(stderr, "rm: %s: is a directory\n", path);
    return EXIT_FAILURE;
  }

  DIR *dir = opendir(path);
  if (dir == NULL) {
    perror(path);
    return EXIT_FAILURE;
  }

  int rc = EXIT_SUCCESS;
  for (;;) {
    errno = 0;
    struct dirent *ent = readdir(dir);
    if (ent == NULL) {
      if (errno != 0) {
        perror(path);
        rc = EXIT_FAILURE;
      }
      break;
    }
    if (streq_local(ent->d_name, ".") || streq_local(ent->d_name, "..")) { continue; }
    char child[512];
    if (!path_join(child, sizeof(child), path, ent->d_name)) {
      fprintf(stderr, "rm: path too long: %s/%s\n", path, ent->d_name);
      rc = EXIT_FAILURE;
      continue;
    }
    if (remove_path(child, opts) != EXIT_SUCCESS) { rc = EXIT_FAILURE; }
  }
  closedir(dir);

  if (rc != EXIT_SUCCESS) { return rc; }
  if (unlink(path) != 0) {
    perror(path);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  struct rm_options opts = {0};
  int first = 1;
  for (; first < argc; ++first) {
    if (strcmp(argv[first], "--") == 0) {
      ++first;
      break;
    }
    if (argv[first][0] != '-' || argv[first][1] == '\0') { break; }
    for (const char *p = argv[first] + 1; *p != '\0'; ++p) {
      if (*p == 'f') {
        opts.force = true;
      } else if (*p == 'r' || *p == 'R') {
        opts.recursive = true;
      } else {
        fprintf(stderr, "rm: invalid option: -%c\n", *p);
        return EXIT_FAILURE;
      }
    }
  }

  if (first == argc) {
    if (opts.force) { return EXIT_SUCCESS; }
    fprintf(stderr, "rm: missing operand\n");
    return EXIT_FAILURE;
  }

  int rc = EXIT_SUCCESS;
  for (int i = first; i < argc; ++i) {
    if (remove_path(argv[i], &opts) != EXIT_SUCCESS) { rc = EXIT_FAILURE; }
  }
  return rc;
}
