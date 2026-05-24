userland_sudo = custom_target('userland-sudo',
  input: files('main.c') + userland_libspore,
  output: 'sudo',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_sudo
