#include "msh.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool append_char(char *word, size_t *len, char c) {
  if (*len + 1 >= WORD_CAP) { return false; }
  word[(*len)++] = c;
  word[*len] = '\0';
  return true;
}

static bool append_text(char *word, size_t *len, const char *text) {
  while (*text != '\0') {
    if (!append_char(word, len, *text++)) { return false; }
  }
  return true;
}

static const char *var_value(const char *name, int last_status) {
  static char status_buf[16];
  if (streq(name, "?")) {
    snprintf(status_buf, sizeof(status_buf), "%d", last_status);
    return status_buf;
  }
  const char *value = getenv(name);
  return value == NULL ? "" : value;
}

static const char *read_variable(const char *p, char *name, size_t cap) {
  size_t len = 0;
  if (*p == '?') {
    snprintf(name, cap, "?");
    return p + 1;
  }
  if (*p == '{') {
    ++p;
    while (*p != '\0' && *p != '}' && len + 1 < cap) {
      name[len++] = *p++;
    }
    name[len] = '\0';
    return *p == '}' ? p + 1 : p;
  }
  while ((*p == '_' || isalnum((unsigned char)*p)) && len + 1 < cap) {
    name[len++] = *p++;
  }
  name[len] = '\0';
  return p;
}

static bool token_add(struct token *tokens, size_t *count, enum token_type type, const char *text) {
  if (*count >= TOKEN_CAP) { return false; }
  tokens[*count].type = type;
  snprintf(tokens[*count].text, sizeof(tokens[*count].text), "%s", text == NULL ? "" : text);
  ++*count;
  return true;
}

static bool expand_tilde(char *word, size_t cap) {
  if (word[0] != '~' || (word[1] != '\0' && word[1] != '/')) { return true; }
  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') { home = "/"; }
  const char *suffix = word[1] == '/' ? word + 1 : "";
  char expanded[WORD_CAP];
  int n = snprintf(expanded, sizeof(expanded), "%s%s", home, suffix);
  if (n < 0 || (size_t)n >= cap) { return false; }
  snprintf(word, cap, "%s", expanded);
  return true;
}

int sh_tokenize(char *line, struct token *tokens, size_t *count, int last_status) {
  const char *p = line;
  *count = 0;
  while (*p != '\0') {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
      ++p;
    }
    if (*p == '\0' || *p == '#') { break; }
    if (*p == '&' && p[1] == '&') {
      if (!token_add(tokens, count, TOK_AND, NULL)) { return -1; }
      p += 2;
      continue;
    }
    if (*p == '&') {
      if (!token_add(tokens, count, TOK_BG, NULL)) { return -1; }
      ++p;
      continue;
    }
    if (*p == '|' && p[1] == '|') {
      if (!token_add(tokens, count, TOK_OR, NULL)) { return -1; }
      p += 2;
      continue;
    }
    if (*p == '|') {
      if (!token_add(tokens, count, TOK_PIPE, NULL)) { return -1; }
      ++p;
      continue;
    }
    if (*p == ';') {
      if (!token_add(tokens, count, TOK_SEMI, NULL)) { return -1; }
      ++p;
      continue;
    }
    if (isdigit((unsigned char)*p)) {
      const char *q = p;
      while (isdigit((unsigned char)*q)) {
        ++q;
      }
      if (*q == '>' || *q == '<') {
        char io[16];
        size_t n = (size_t)(q - p);
        if (n >= sizeof(io)) { return -1; }
        memcpy(io, p, n);
        io[n] = '\0';
        if (!token_add(tokens, count, TOK_IO_NUMBER, io)) { return -1; }
        p = q;
        continue;
      }
    }
    if (*p == '>') {
      enum token_type type = p[1] == '>' ? TOK_GTGT : TOK_GT;
      if (!token_add(tokens, count, type, NULL)) { return -1; }
      p += type == TOK_GTGT ? 2 : 1;
      continue;
    }
    if (*p == '<') {
      if (!token_add(tokens, count, TOK_LT, NULL)) { return -1; }
      ++p;
      continue;
    }

    char word[WORD_CAP] = {0};
    size_t len = 0;
    bool tilde_eligible = false;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != ';' && *p != '&' && *p != '|' &&
           *p != '<' && *p != '>' && !(*p == '&' && p[1] == '&')) {
      if (*p == '\'') {
        ++p;
        while (*p != '\0' && *p != '\'') {
          if (!append_char(word, &len, *p++)) { return -1; }
        }
        if (*p == '\'') { ++p; }
      } else if (*p == '"') {
        ++p;
        while (*p != '\0' && *p != '"') {
          if (*p == '\\' && p[1] != '\0') {
            ++p;
            if (!append_char(word, &len, *p++)) { return -1; }
          } else if (*p == '$') {
            char name[64];
            p = read_variable(p + 1, name, sizeof(name));
            if (!append_text(word, &len, var_value(name, last_status))) { return -1; }
          } else if (!append_char(word, &len, *p++)) {
            return -1;
          }
        }
        if (*p == '"') { ++p; }
      } else if (*p == '\\' && p[1] != '\0') {
        ++p;
        if (!append_char(word, &len, *p++)) { return -1; }
      } else if (*p == '$') {
        char name[64];
        const char *next = read_variable(p + 1, name, sizeof(name));
        if (next == p + 1) {
          if (!append_char(word, &len, *p++)) { return -1; }
        } else {
          p = next;
          if (!append_text(word, &len, var_value(name, last_status))) { return -1; }
        }
      } else {
        if (len == 0 && *p == '~') { tilde_eligible = true; }
        if (!append_char(word, &len, *p++)) { return -1; }
      }
    }
    if (tilde_eligible && !expand_tilde(word, sizeof(word))) { return -1; }
    if (!token_add(tokens, count, TOK_WORD, word)) { return -1; }
  }
  return token_add(tokens, count, TOK_END, NULL) ? 0 : -1;
}

enum token_type sh_parser_peek(struct parser *p) {
  return p->pos < p->count ? p->tokens[p->pos].type : TOK_END;
}

struct token *sh_parser_take(struct parser *p) {
  return p->pos < p->count ? &p->tokens[p->pos++] : NULL;
}

static bool decimal_word(const char *s) {
  if (s == NULL || s[0] == '\0') { return false; }
  for (const char *p = s; *p != '\0'; ++p) {
    if (!isdigit((unsigned char)*p)) { return false; }
  }
  return true;
}

static bool assignment_syntax(const char *word) {
  const char *eq = strchr(word, '=');
  if (eq == NULL || eq == word) { return false; }
  for (const char *p = word; p < eq; ++p) {
    bool first = p == word;
    if (!(*p == '_' || (!first && *p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) {
      return false;
    }
  }
  return true;
}

static int add_redir(struct command *cmd, enum redir_op op, int fd, const char *path, int dup_fd) {
  if (cmd->redir.count >= REDIR_CAP) {
    eprintf("sh: too many redirections\n");
    return -1;
  }
  cmd->redir.actions[cmd->redir.count++] = (struct redir_action){.op = op, .fd = fd, .dup_fd = dup_fd, .path = path};
  return 0;
}

int sh_parse_command(struct parser *p, struct command *cmd) {
  memset(cmd, 0, sizeof(*cmd));
  while (sh_parser_peek(p) != TOK_END && sh_parser_peek(p) != TOK_AND && sh_parser_peek(p) != TOK_OR &&
         sh_parser_peek(p) != TOK_PIPE && sh_parser_peek(p) != TOK_SEMI && sh_parser_peek(p) != TOK_BG) {
    struct token *tok = sh_parser_take(p);
    if (tok->type == TOK_WORD) {
      if (cmd->argc == 0 && assignment_syntax(tok->text)) {
        if (cmd->envc + 1 >= ARG_CAP) {
          eprintf("sh: too many environment assignments\n");
          return -1;
        }
        cmd->env[cmd->envc++] = tok->text;
        cmd->env[cmd->envc] = NULL;
        continue;
      }
      if (cmd->argc + 1 >= ARG_CAP) {
        eprintf("sh: too many arguments\n");
        return -1;
      }
      cmd->argv[cmd->argc++] = tok->text;
      cmd->argv[cmd->argc] = NULL;
    } else if (tok->type == TOK_IO_NUMBER || tok->type == TOK_GT || tok->type == TOK_GTGT || tok->type == TOK_LT) {
      int fd = -1;
      if (tok->type == TOK_IO_NUMBER) {
        fd = atoi(tok->text);
        tok = sh_parser_take(p);
        if (tok == NULL || (tok->type != TOK_GT && tok->type != TOK_GTGT && tok->type != TOK_LT)) {
          eprintf("sh: syntax error\n");
          return -1;
        }
      }
      if (fd < 0) { fd = tok->type == TOK_LT ? STDIN_FILENO : STDOUT_FILENO; }
      enum redir_op op =
        tok->type == TOK_LT ? REDIR_OPEN_READ : (tok->type == TOK_GTGT ? REDIR_OPEN_APPEND : REDIR_OPEN_WRITE);
      if (sh_parser_peek(p) == TOK_BG && tok->type != TOK_LT) {
        (void)sh_parser_take(p);
        struct token *target = sh_parser_take(p);
        if (target == NULL || target->type != TOK_WORD || !decimal_word(target->text)) {
          eprintf("sh: redirection missing fd\n");
          return -1;
        }
        if (add_redir(cmd, REDIR_DUP, fd, NULL, atoi(target->text)) != 0) { return -1; }
        continue;
      }
      struct token *path = sh_parser_take(p);
      if (path == NULL || path->type != TOK_WORD) {
        eprintf("sh: redirection missing path\n");
        return -1;
      }
      if (add_redir(cmd, op, fd, path->text, -1) != 0) { return -1; }
    } else {
      eprintf("sh: syntax error\n");
      return -1;
    }
  }
  return 0;
}
