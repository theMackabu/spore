lexer_archive = custom_target('msh-lexer-rust',
  input: 'lexer.rs',
  depend_files: userland_rust_panic,
  output: 'libmsh_lexer.a',
  command: userland_rust_archive + ['@INPUT@', '-o', '@OUTPUT@'])

userland_msh = custom_target('userland-msh',
  input: files('main.c', 'alias.c', 'input.c', 'history.c', 'parser.c', 'eval.c', 'job.c') + userland_libspore + [lexer_archive],
  depend_files: files('msh.h'),
  output: 'msh',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_msh
