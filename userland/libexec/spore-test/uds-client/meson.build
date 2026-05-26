userland_uds_client = custom_target('userland-uds-client',
  input: files('main.c') + userland_libspore,
  output: 'uds-client',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_uds_client
