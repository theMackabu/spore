userland_memhog = custom_target('userland-memhog',
  input: files('main.c') + userland_libspore,
  output: 'memhog',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_memhog
