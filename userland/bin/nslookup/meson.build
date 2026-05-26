userland_nslookup = custom_target('userland-nslookup',
  input: files('main.c') + userland_libspore,
  output: 'nslookup',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_nslookup
