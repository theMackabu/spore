userland_true = custom_target('userland-true',
  input: files('main.c') + userland_libspore,
  output: 'true',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_true
