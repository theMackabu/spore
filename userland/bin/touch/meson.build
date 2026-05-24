userland_touch = custom_target('userland-touch',
  input: files('main.c') + userland_libspore,
  output: 'touch',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_touch
