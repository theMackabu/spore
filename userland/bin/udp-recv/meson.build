userland_udp_recv = custom_target('userland-udp-recv',
  input: files('main.c') + userland_libspore,
  output: 'udp-recv',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_udp_recv
