userland_peeker = custom_target('userland-peeker',
  input: files('main.c') + userland_libspore,
  output: 'peeker',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_peeker
