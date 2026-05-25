userland_env = custom_target('userland-env',
  input: files('main.c') + userland_libspore,
  output: 'env',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_env
