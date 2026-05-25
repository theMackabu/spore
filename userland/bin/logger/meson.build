userland_logger = custom_target('userland-logger',
  input: files('main.c') + userland_libspore,
  output: 'logger',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_logger
