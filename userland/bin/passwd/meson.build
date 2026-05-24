userland_passwd = custom_target('userland-passwd',
  input: files('main.c') + userland_libspore,
  output: 'passwd',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_passwd
