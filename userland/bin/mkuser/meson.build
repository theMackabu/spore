userland_mkuser = custom_target('userland-mkuser',
  input: files('main.c') + userland_libspore,
  output: 'mkuser',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_mkuser
