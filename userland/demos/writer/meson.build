userland_writer = custom_target('userland-writer',
  input: files('main.c') + userland_libspore,
  output: 'writer',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_writer
