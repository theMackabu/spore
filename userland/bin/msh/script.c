#include "msh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  SCRIPT_LINE_CAP = 512,
  SCRIPT_LOOP_LIMIT = 100000,
};

static void chomp_line(char *line) {
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
    line[--len] = '\0';
  }
}

static char *trim_line(char *line) {
  while (*line == ' ' || *line == '\t') {
    ++line;
  }
  char *end = line + strlen(line);
  while (end > line && (end[-1] == ' ' || end[-1] == '\t')) {
    *--end = '\0';
  }
  return line;
}

static bool script_blank_or_comment(const char *line) {
  while (*line == ' ' || *line == '\t') {
    ++line;
  }
  return *line == '\0' || *line == '#';
}

static bool script_word_is(const char *line, const char *word) {
  line += strspn(line, " \t");
  size_t n = strlen(word);
  return strncmp(line, word, n) == 0 && line[n + strspn(line + n, " \t")] == '\0';
}

static bool script_starts_keyword(const char *line, const char *keyword) {
  line += strspn(line, " \t");
  size_t n = strlen(keyword);
  return strncmp(line, keyword, n) == 0 && (line[n] == '\0' || line[n] == ' ' || line[n] == '\t');
}

static bool strip_header_term(char *cmd, const char *term) {
  bool term_stripped = false;
  char *p = trim_line(cmd);
  memmove(cmd, p, strlen(p) + 1);
  size_t term_len = strlen(term);
  for (;;) {
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t')) {
      cmd[--len] = '\0';
    }
    if (len > 0 && cmd[len - 1] == ';') {
      cmd[--len] = '\0';
      continue;
    }
    if (len >= term_len && strcmp(cmd + len - term_len, term) == 0 &&
        (len == term_len || cmd[len - term_len - 1] == ' ' || cmd[len - term_len - 1] == '\t' ||
         cmd[len - term_len - 1] == ';')) {
      cmd[len - term_len] = '\0';
      term_stripped = true;
      continue;
    }
    break;
  }
  p = trim_line(cmd);
  memmove(cmd, p, strlen(p) + 1);
  return term_stripped;
}

static bool extract_header_command(char *dst, size_t cap, const char *line, const char *keyword, const char *term,
                                   bool *inline_term) {
  const char *p = line + strspn(line, " \t") + strlen(keyword);
  p += strspn(p, " \t");
  snprintf(dst, cap, "%s", p);
  *inline_term = strip_header_term(dst, term);
  return dst[0] != '\0';
}

static int execute_script_range(char *const lines[], size_t start, size_t end, int status);

static int execute_script_if(char *const lines[], size_t *index, size_t end, int status) {
  size_t header = *index;
  *index = end;
  char condition[LINE_CAP];
  bool inline_then = false;
  if (!extract_header_command(condition, sizeof(condition), lines[header], "if", "then", &inline_then)) {
    eprintf("msh: bad if\n");
    return 2;
  }

  size_t body_start = header + 1;
  if (!inline_then) {
    while (body_start < end && script_blank_or_comment(lines[body_start])) {
      ++body_start;
    }
    if (body_start >= end || !script_word_is(lines[body_start], "then")) {
      eprintf("msh: if without then\n");
      return 2;
    }
    ++body_start;
  }

  size_t depth = 0;
  size_t else_pos = end;
  size_t fi_pos = end;
  for (size_t i = body_start; i < end; ++i) {
    if (script_blank_or_comment(lines[i])) { continue; }
    if (script_starts_keyword(lines[i], "if")) {
      ++depth;
      continue;
    }
    if (script_word_is(lines[i], "fi")) {
      if (depth == 0) {
        fi_pos = i;
        break;
      }
      --depth;
      continue;
    }
    if (depth == 0 && script_word_is(lines[i], "else")) { else_pos = i; }
  }
  if (fi_pos == end) {
    eprintf("msh: if without fi\n");
    return 2;
  }

  int cond = sh_execute_line(condition, status);
  if (cond == 0) {
    status = execute_script_range(lines, body_start, else_pos < fi_pos ? else_pos : fi_pos, cond);
  } else if (else_pos < fi_pos) {
    status = execute_script_range(lines, else_pos + 1, fi_pos, cond);
  } else {
    status = cond;
  }
  *index = fi_pos + 1;
  return status;
}

static int execute_script_while(char *const lines[], size_t *index, size_t end, int status) {
  size_t header = *index;
  *index = end;
  char condition[LINE_CAP];
  bool inline_do = false;
  if (!extract_header_command(condition, sizeof(condition), lines[header], "while", "do", &inline_do)) {
    eprintf("msh: bad while\n");
    return 2;
  }

  size_t body_start = header + 1;
  if (!inline_do) {
    while (body_start < end && script_blank_or_comment(lines[body_start])) {
      ++body_start;
    }
    if (body_start >= end || !script_word_is(lines[body_start], "do")) {
      eprintf("msh: while without do\n");
      return 2;
    }
    ++body_start;
  }

  size_t depth = 0;
  size_t done_pos = end;
  for (size_t i = body_start; i < end; ++i) {
    if (script_blank_or_comment(lines[i])) { continue; }
    if (script_starts_keyword(lines[i], "while")) {
      ++depth;
      continue;
    }
    if (script_word_is(lines[i], "done")) {
      if (depth == 0) {
        done_pos = i;
        break;
      }
      --depth;
    }
  }
  if (done_pos == end) {
    eprintf("msh: while without done\n");
    return 2;
  }

  for (int loops = 0; loops < SCRIPT_LOOP_LIMIT; ++loops) {
    int cond = sh_execute_line(condition, status);
    if (cond != 0) {
      status = cond;
      break;
    }
    status = execute_script_range(lines, body_start, done_pos, cond);
  }
  *index = done_pos + 1;
  return status;
}

static int execute_script_range(char *const lines[], size_t start, size_t end, int status) {
  size_t i = start;
  while (i < end) {
    if (script_blank_or_comment(lines[i])) {
      ++i;
      continue;
    }
    if (script_starts_keyword(lines[i], "if")) {
      status = execute_script_if(lines, &i, end, status);
      continue;
    }
    if (script_starts_keyword(lines[i], "while")) {
      status = execute_script_while(lines, &i, end, status);
      continue;
    }
    if (script_word_is(lines[i], "then") || script_word_is(lines[i], "else") || script_word_is(lines[i], "fi") ||
        script_word_is(lines[i], "do") || script_word_is(lines[i], "done")) {
      eprintf("msh: unexpected block marker\n");
      return 2;
    }
    status = sh_execute_line(lines[i], status);
    ++i;
  }
  return status;
}

int sh_source_file(const char *path, int last_status, bool complain) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    if (complain) { perror(path); }
    return complain ? 1 : last_status;
  }

  char *lines[SCRIPT_LINE_CAP];
  char line[LINE_CAP];
  size_t count = 0;
  int status = last_status;
  while (fgets(line, sizeof(line), file) != NULL) {
    if (count == SCRIPT_LINE_CAP) {
      eprintf("%s: script too large\n", path);
      status = 2;
      goto cleanup;
    }
    chomp_line(line);
    lines[count] = strdup(line);
    if (lines[count] == NULL) {
      perror("msh");
      status = 1;
      goto cleanup;
    }
    ++count;
  }
  if (ferror(file)) {
    perror(path);
    status = 1;
    goto cleanup;
  }
  fclose(file);
  file = NULL;
  status = execute_script_range(lines, 0, count, status);
cleanup:
  if (file != NULL) { fclose(file); }
  for (size_t i = 0; i < count; ++i) { free(lines[i]); }
  return status;
}
