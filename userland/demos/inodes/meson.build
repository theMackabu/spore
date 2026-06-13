userland_inodes = custom_target('userland-inodes',
  input: files('main.c') + userland_libspore,
  output: 'inodes',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_inodes
