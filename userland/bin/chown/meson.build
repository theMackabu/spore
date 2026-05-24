userland_chown = custom_target('userland-chown',
  input: files('main.c') + userland_libspore,
  output: 'chown',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_chown
