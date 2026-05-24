#pragma once

#include <spore.h>
#include <stdbool.h>
#include <stddef.h>

extern char **environ;

#define SYS_SPORE_APPLY_POLICY 0x4005

enum {
  LINE_CAP = 1024,
  WORD_CAP = 256,
  TOKEN_CAP = 96,
  ARG_CAP = 32,
  HISTORY_CAP = 16,
};

enum token_type {
  TOK_END,
  TOK_WORD,
  TOK_BG,
  TOK_AND,
  TOK_OR,
  TOK_SEMI,
  TOK_GT,
  TOK_GTGT,
  TOK_LT,
};

struct token {
  enum token_type type;
  char text[WORD_CAP];
};

struct redirs {
  const char *in;
  const char *out;
  bool append;
};

struct command {
  char *argv[ARG_CAP];
  int argc;
  struct redirs redir;
  bool background;
};

struct parser {
  struct token *tokens;
  size_t pos;
  size_t count;
};

int sh_read_line(char *buf, size_t cap);
void sh_reap_jobs(bool verbose);
int sh_tokenize(char *line, struct token *tokens, size_t *count, int last_status);
enum token_type sh_parser_peek(struct parser *p);
struct token *sh_parser_take(struct parser *p);
int sh_parse_command(struct parser *p, struct command *cmd);
int sh_execute_line(char *line, int last_status);
