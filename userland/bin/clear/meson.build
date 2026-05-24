userland_clear = custom_target('userland-clear',
  input: files('main.c') + userland_libspore,
  output: 'clear',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_clear
