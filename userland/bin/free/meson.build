userland_free = custom_target('userland-free',
  input: files('main.c') + userland_libspore,
  output: 'free',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_free
