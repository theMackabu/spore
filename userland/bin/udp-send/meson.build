userland_udp_send = custom_target('userland-udp-send',
  input: files('main.c') + userland_libspore,
  output: 'udp-send',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_udp_send
