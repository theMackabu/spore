#include <dirent.h>
#include <errno.h>
#include <spore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum {
  MAX_ENTRIES = 512,
  NAME_CAP = 256,
  PATH_CAP = 512,
};

struct ls_options {
  bool all;
  bool long_format;
  bool human;
  bool reverse;
  bool sort_time;
  bool color;
};

struct file_info {
  char name[NAME_CAP];
  char path[PATH_CAP];
  struct stat st;
};

static const struct ls_options *g_sort_options;

static int copy_string(char *dst, size_t cap, const char *src) {
  int n = snprintf(dst, cap, "%s", src);
  return n >= 0 && n < (int)cap ? 0 : -1;
}

static int join_path(char *out, size_t cap, const char *dir, const char *name) {
  int n = streq(dir, "/") ? snprintf(out, cap, "/%s", name) : snprintf(out, cap, "%s/%s", dir, name);
  return n >= 0 && n < (int)cap ? 0 : -1;
}

static const char *leaf_name(const char *path) {
  const char *slash = strrchr(path, '/');
  if (slash == NULL || slash[1] == '\0') { return path; }
  return slash + 1;
}

static bool hidden_name(const char *name) {
  return name[0] == '.';
}

static unsigned long long blocks_for(const struct stat *st) {
  if (st->st_blocks > 0) { return (unsigned long long)st->st_blocks; }
  return ((unsigned long long)st->st_size + 511ull) / 512ull;
}

static const char *entry_color(mode_t mode) {
  if (S_ISDIR(mode)) { return "\033[01;34m"; }
  if (S_ISLNK(mode)) { return "\033[01;36m"; }
  if (S_ISFIFO(mode)) { return "\033[33m"; }
  if (S_ISSOCK(mode)) { return "\033[01;35m"; }
  if (S_ISBLK(mode) || S_ISCHR(mode)) { return "\033[01;33m"; }
  if (S_ISREG(mode) && (mode & 0111) != 0) { return "\033[01;32m"; }
  return "";
}

static void print_name_colored(const struct file_info *entry, const struct ls_options *options) {
  const char *color = options->color ? entry_color(entry->st.st_mode) : "";
  if (color[0] != '\0') {
    printf("%s%s\033[0m", color, entry->name);
  } else {
    fputs(entry->name, stdout);
  }
}

static void format_mode(mode_t mode, char out[11]) {
  out[0] = '-';
  if (S_ISDIR(mode)) {
    out[0] = 'd';
  } else if (S_ISLNK(mode)) {
    out[0] = 'l';
  } else if (S_ISCHR(mode)) {
    out[0] = 'c';
  } else if (S_ISBLK(mode)) {
    out[0] = 'b';
  } else if (S_ISFIFO(mode)) {
    out[0] = 'p';
  } else if (S_ISSOCK(mode)) {
    out[0] = 's';
  }
  const mode_t masks[] = {0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001};
  const char chars[] = {'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x'};
  for (size_t i = 0; i < 9; ++i) {
    out[i + 1] = (mode & masks[i]) != 0 ? chars[i] : '-';
  }
  out[10] = '\0';
}

static void format_size(unsigned long long size, bool human, char *out, size_t cap) {
  if (!human) {
    snprintf(out, cap, "%llu", size);
    return;
  }
  static const char *units[] = {"B", "K", "M", "G", "T"};
  unsigned unit = 0;
  unsigned long long whole = size;
  while (whole >= 1024 && unit + 1 < sizeof(units) / sizeof(units[0])) {
    whole = (whole + 512) / 1024;
    ++unit;
  }
  snprintf(out, cap, "%llu%s", whole, units[unit]);
}

static void format_time(time_t t, char *out, size_t cap) {
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  struct tm *tm = localtime(&t);
  if (tm == NULL) {
    snprintf(out, cap, "1970 Jan 01 00:00");
    return;
  }
  snprintf(out, cap, "%04d %s %02d %02d:%02d", tm->tm_year + 1900, months[tm->tm_mon], tm->tm_mday, tm->tm_hour,
           tm->tm_min);
}

static int compare_entries(const void *lhs, const void *rhs) {
  const struct file_info *a = lhs;
  const struct file_info *b = rhs;
  int order;
  if (g_sort_options != NULL && g_sort_options->sort_time) {
    if (a->st.st_mtime < b->st.st_mtime) {
      order = 1;
    } else if (a->st.st_mtime > b->st.st_mtime) {
      order = -1;
    } else {
      order = strcmp(a->name, b->name);
    }
  } else {
    order = strcmp(a->name, b->name);
  }
  return g_sort_options != NULL && g_sort_options->reverse ? -order : order;
}

static bool has_entry(const struct file_info *entries, size_t count, const char *name) {
  for (size_t i = 0; i < count; ++i) {
    if (streq(entries[i].name, name)) { return true; }
  }
  return false;
}

static int add_entry(struct file_info *entries, size_t *count, const char *dir, const char *name,
                     const char *stat_path) {
  if (*count >= MAX_ENTRIES) {
    eprintf("ls: too many entries in %s\n", dir);
    return -1;
  }
  struct file_info *entry = &entries[*count];
  if (copy_string(entry->name, sizeof(entry->name), name) != 0 ||
      copy_string(entry->path, sizeof(entry->path), stat_path) != 0) {
    eprintf("ls: path too long: %s\n", name);
    return -1;
  }
  if (lstat(stat_path, &entry->st) != 0) {
    perror(stat_path);
    return -1;
  }
  ++*count;
  return 0;
}

static int read_directory(const char *path, const struct ls_options *options, struct file_info *entries,
                          size_t *count) {
  *count = 0;
  DIR *dir = opendir(path);
  if (dir == NULL) {
    perror(path);
    return -1;
  }

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (!options->all && hidden_name(ent->d_name)) { continue; }
    char child[PATH_CAP];
    if (join_path(child, sizeof(child), path, ent->d_name) != 0) {
      eprintf("ls: path too long: %s/%s\n", path, ent->d_name);
      closedir(dir);
      return -1;
    }
    if (add_entry(entries, count, path, ent->d_name, child) != 0) {
      closedir(dir);
      return -1;
    }
  }
  closedir(dir);

  if (options->all) {
    if (!has_entry(entries, *count, ".")) { (void)add_entry(entries, count, path, ".", path); }
    if (!has_entry(entries, *count, "..")) {
      char parent[PATH_CAP];
      if (join_path(parent, sizeof(parent), path, "..") == 0) { (void)add_entry(entries, count, path, "..", parent); }
    }
  }

  g_sort_options = options;
  qsort(entries, *count, sizeof(entries[0]), compare_entries);
  return 0;
}

static void print_long_entry(const struct file_info *entry, const struct ls_options *options) {
  char mode[11];
  char size[32];
  char when[32];
  format_mode(entry->st.st_mode, mode);
  format_size((unsigned long long)entry->st.st_size, options->human, size, sizeof(size));
  format_time(entry->st.st_mtime, when, sizeof(when));
  printf("%s %3lu %5u %5u %6s %s ", mode, (unsigned long)entry->st.st_nlink, (unsigned)entry->st.st_uid,
         (unsigned)entry->st.st_gid, size, when);
  print_name_colored(entry, options);
  if (S_ISLNK(entry->st.st_mode)) {
    char target[PATH_CAP];
    ssize_t n = readlink(entry->path, target, sizeof(target) - 1);
    if (n >= 0) {
      target[n] = '\0';
      printf(" -> %s", target);
    }
  }
  putchar('\n');
}

static void display_entries(const struct file_info *entries, size_t count, const struct ls_options *options) {
  if (options->long_format) {
    unsigned long long total = 0;
    for (size_t i = 0; i < count; ++i) {
      total += blocks_for(&entries[i].st);
    }
    printf("total %llu\n", total);
    for (size_t i = 0; i < count; ++i) {
      print_long_entry(&entries[i], options);
    }
    return;
  }

  if (count == 0) { return; }

  size_t max_width = 0;
  for (size_t i = 0; i < count; ++i) {
    size_t len = strlen(entries[i].name);
    if (len > max_width) { max_width = len; }
  }
  size_t column_width = max_width + 2;
  if (column_width == 0) { column_width = 1; }
  size_t terminal_width = 80;
  const char *columns_env = getenv("COLUMNS");
  if (columns_env != NULL && columns_env[0] != '\0') {
    unsigned long parsed = strtoul(columns_env, NULL, 10);
    if (parsed > 0) { terminal_width = parsed; }
  }
  size_t columns = terminal_width / column_width;
  if (columns == 0) { columns = 1; }
  if (columns > count) { columns = count; }
  size_t rows = (count + columns - 1) / columns;

  for (size_t row = 0; row < rows; ++row) {
    for (size_t col = 0; col < columns; ++col) {
      size_t index = col * rows + row;
      if (index >= count) { continue; }
      bool last = index + rows >= count;
      print_name_colored(&entries[index], options);
      if (!last) {
        size_t name_len = strlen(entries[index].name);
        size_t pad = column_width > name_len ? column_width - name_len : 1;
        for (size_t i = 0; i < pad; ++i) {
          putchar(' ');
        }
      }
    }
    putchar('\n');
  }
}

static int list_one(const char *path, const struct ls_options *options) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    perror(path);
    return EXIT_FAILURE;
  }

  struct file_info entries[MAX_ENTRIES];
  size_t count = 0;
  if (S_ISDIR(st.st_mode)) {
    if (read_directory(path, options, entries, &count) != 0) { return EXIT_FAILURE; }
  } else {
    if (copy_string(entries[0].name, sizeof(entries[0].name), leaf_name(path)) != 0 ||
        copy_string(entries[0].path, sizeof(entries[0].path), path) != 0) {
      eprintf("ls: path too long: %s\n", path);
      return EXIT_FAILURE;
    }
    entries[0].st = st;
    count = 1;
  }

  display_entries(entries, count, options);
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  struct ls_options options = {.color = isatty(STDOUT_FILENO)};
  const char *paths[64];
  int path_count = 0;

  for (int i = 1; i < argc; ++i) {
    if (streq(argv[i], "--help")) { return usage("ls", "[-alhrt] [file ...]"); }
    if (streq(argv[i], "--all")) {
      options.all = true;
      continue;
    }
    if (argv[i][0] == '-' && argv[i][1] != '\0') {
      for (const char *p = argv[i] + 1; *p != '\0'; ++p) {
        if (*p == 'a') {
          options.all = true;
        } else if (*p == 'l') {
          options.long_format = true;
        } else if (*p == 'h') {
          options.human = true;
        } else if (*p == 'r') {
          options.reverse = true;
        } else if (*p == 't') {
          options.sort_time = true;
        } else {
          return usage("ls", "[-alhrt] [file ...]");
        }
      }
      continue;
    }
    if (path_count >= (int)(sizeof(paths) / sizeof(paths[0]))) {
      eprintf("ls: too many paths\n");
      return EXIT_FAILURE;
    }
    paths[path_count++] = argv[i];
  }

  if (path_count == 0) { paths[path_count++] = "."; }

  int rc = EXIT_SUCCESS;
  for (int i = 0; i < path_count; ++i) {
    if (path_count > 1) { printf("%s:\n", paths[i]); }
    if (list_one(paths[i], &options) != EXIT_SUCCESS) { rc = EXIT_FAILURE; }
    if (path_count > 1 && i + 1 < path_count) { putchar('\n'); }
  }
  return rc;
}
