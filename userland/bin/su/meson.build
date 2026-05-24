userland_su = custom_target('userland-su',
  input: files('main.c') + userland_libspore,
  output: 'su',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_su
