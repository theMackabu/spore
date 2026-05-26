userland_more = custom_target('userland-more',
  input: files('main.c') + userland_libspore,
  output: 'more',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_more
