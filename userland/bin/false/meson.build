userland_false = custom_target('userland-false',
  input: files('main.c') + userland_libspore,
  output: 'false',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_false
