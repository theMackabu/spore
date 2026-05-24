userland_pwd = custom_target('userland-pwd',
  input: files('main.c') + userland_libspore,
  output: 'pwd',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_pwd
