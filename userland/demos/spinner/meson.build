userland_spinner = custom_target('userland-spinner',
  input: files('main.c') + userland_libspore,
  output: 'spinner',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_spinner
