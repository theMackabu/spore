userland_mkdir = custom_target('userland-mkdir',
  input: files('main.c') + userland_libspore,
  output: 'mkdir',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_mkdir
