userland_msh = custom_target('userland-msh',
  input: files('main.c', 'input.c', 'history.c', 'lexer.c', 'eval.c', 'job.c') + userland_libspore,
  output: 'msh',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_msh
