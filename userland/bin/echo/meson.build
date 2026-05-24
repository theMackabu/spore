userland_echo = custom_target('userland-echo',
  input: files('main.c') + userland_libspore,
  output: 'echo',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_echo
