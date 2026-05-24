userland_udp_echo = custom_target('userland-udp-echo',
  input: files('main.c') + userland_libspore,
  output: 'udp-echo',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_udp_echo
