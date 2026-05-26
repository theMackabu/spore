userland_less = custom_target('userland-less',
  input: files('main.c') + userland_libspore,
  output: 'less',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_less
