userland_rm = custom_target('userland-rm',
  input: files('main.c') + userland_libspore,
  output: 'rm',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_rm
