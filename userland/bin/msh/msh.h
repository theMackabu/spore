#pragma once

#include <spore.h>
#include <stdbool.h>
#include <stddef.h>

extern char **environ;

#define SYS_SPORE_APPLY_POLICY 0x4005

enum {
  LINE_CAP = 8192,
  WORD_CAP = 4096,
  TOKEN_CAP = 256,
  ARG_CAP = 128,
  HISTORY_CAP = 16,
};

enum token_type {
  TOK_END,
  TOK_WORD,
  TOK_BG,
  TOK_AND,
  TOK_OR,
  TOK_PIPE,
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
void sh_expand_prompt(const char *src, char *out, size_t cap);
void sh_history_load(void);
void sh_history_add(const char *line);
size_t sh_history_count(void);
const char *sh_history_get(size_t index);
void sh_reap_jobs(bool verbose);
int sh_tokenize(char *line, struct token *tokens, size_t *count, int last_status);
enum token_type sh_parser_peek(struct parser *p);
struct token *sh_parser_take(struct parser *p);
int sh_parse_command(struct parser *p, struct command *cmd);
int sh_execute_line(char *line, int last_status);
int sh_source_file(const char *path, int last_status, bool complain);
