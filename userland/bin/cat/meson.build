userland_cat = custom_target('userland-cat',
  input: files('main.c') + userland_libspore,
  output: 'cat',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_cat
