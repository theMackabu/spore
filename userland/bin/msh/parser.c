#include "msh.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static int expand_alias(struct parser *p, int index) {
  const char *value = sh_aliases[index].value;
  uint64_t expanded = p->tokens[p->pos].expanded_aliases | (UINT64_C(1) << index);
  bool after_blank = p->tokens[p->pos].after_alias_blank;
  free(p->tokens[p->pos].text);
  size_t tail = p->count - p->pos - 1;
  struct token *saved = &p->tokens[TOKEN_CAP - tail];
  memmove(saved, &p->tokens[p->pos + 1], tail * sizeof(*saved));
  size_t added;
  int rc = sh_tokenize(value, &p->tokens[p->pos], TOKEN_CAP - tail - p->pos, &added);
  for (size_t i = 0; i < added; ++i) {
    p->tokens[p->pos + i].expanded_aliases = expanded;
  }
  if (added && after_blank) { p->tokens[p->pos].after_alias_blank = true; }
  size_t len = strlen(value);
  if (tail && len && (value[len - 1] == ' ' || value[len - 1] == '\t')) { saved->after_alias_blank = true; }
  memmove(&p->tokens[p->pos + added], saved, tail * sizeof(*saved));
  p->count = p->pos + added + tail;
  if (rc != 0) { eprintf("msh: alias expansion exceeds token limit\n"); }
  return rc;
}

int sh_parse_command(struct parser *p, struct command *cmd, bool execute) {
  memset(cmd, 0, sizeof(*cmd));
  while (sh_parser_peek(p) != TOK_END && sh_parser_peek(p) != TOK_AND && sh_parser_peek(p) != TOK_OR &&
         sh_parser_peek(p) != TOK_PIPE && sh_parser_peek(p) != TOK_SEMI && sh_parser_peek(p) != TOK_BG) {
    struct token *tok = &p->tokens[p->pos];
    if (tok->literal && (cmd->argc == 0 || tok->after_alias_blank)) {
      int index = sh_alias_index(tok->text);
      if (index >= 0 && !(tok->expanded_aliases & (UINT64_C(1) << index))) {
        if (expand_alias(p, index) != 0) { return -1; }
        continue;
      }
    }
    sh_parser_take(p);
    if (execute && sh_expand_word(tok, p->last_status) < 0) {
      eprintf("msh: word expansion too large\n");
      return -1;
    }
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
        if (target == NULL || target->type != TOK_WORD || (execute && (sh_expand_word(target, p->last_status) < 0 || !decimal_word(target->text)))) {
          eprintf("sh: redirection missing fd\n");
          return -1;
        }
        if (add_redir(cmd, REDIR_DUP, fd, NULL, atoi(target->text)) != 0) { return -1; }
        continue;
      }
      struct token *path = sh_parser_take(p);
      if (path == NULL || path->type != TOK_WORD || (execute && sh_expand_word(path, p->last_status) < 0)) {
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
