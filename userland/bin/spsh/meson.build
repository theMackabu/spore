userland_spsh = custom_target('userland-spsh',
  input: files('main.c') + userland_libspore,
  output: 'spsh',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_spsh
