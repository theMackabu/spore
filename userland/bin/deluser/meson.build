userland_deluser = custom_target('userland-deluser',
  input: files('main.c') + userland_libspore,
  output: 'deluser',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_deluser
