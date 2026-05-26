userland_uds_server = custom_target('userland-uds-server',
  input: files('main.c') + userland_libspore,
  output: 'uds-server',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_uds_server
