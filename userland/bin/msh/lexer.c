#include "msh.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int sh_tokenize(char *line, struct token *tokens, size_t *count, int last_status) {
  const char *p = line;
  *count = 0;
  while (*p != '\0') {
    while (*p == ' ' || *p == '\t') {
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
    if (*p == ';') {
      if (!token_add(tokens, count, TOK_SEMI, NULL)) { return -1; }
      ++p;
      continue;
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
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != ';' && *p != '&' && *p != '<' && *p != '>' &&
           !(*p == '&' && p[1] == '&') && !(*p == '|' && p[1] == '|')) {
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
      } else if (!append_char(word, &len, *p++)) {
        return -1;
      }
    }
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

int sh_parse_command(struct parser *p, struct command *cmd) {
  memset(cmd, 0, sizeof(*cmd));
  while (sh_parser_peek(p) != TOK_END && sh_parser_peek(p) != TOK_AND && sh_parser_peek(p) != TOK_OR &&
         sh_parser_peek(p) != TOK_SEMI && sh_parser_peek(p) != TOK_BG) {
    struct token *tok = sh_parser_take(p);
    if (tok->type == TOK_WORD) {
      if (cmd->argc + 1 >= ARG_CAP) {
        eprintf("sh: too many arguments\n");
        return -1;
      }
      cmd->argv[cmd->argc++] = tok->text;
      cmd->argv[cmd->argc] = NULL;
    } else if (tok->type == TOK_GT || tok->type == TOK_GTGT || tok->type == TOK_LT) {
      struct token *path = sh_parser_take(p);
      if (path == NULL || path->type != TOK_WORD) {
        eprintf("sh: redirection missing path\n");
        return -1;
      }
      if (tok->type == TOK_LT) {
        cmd->redir.in = path->text;
      } else {
        cmd->redir.out = path->text;
        cmd->redir.append = tok->type == TOK_GTGT;
      }
    } else {
      eprintf("sh: syntax error\n");
      return -1;
    }
  }
  return 0;
}
