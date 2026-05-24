userland_sh = custom_target('userland-sh',
  input: files('main.c', 'input.c', 'lexer.c', 'eval.c') + userland_libspore,
  output: 'sh',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_sh
