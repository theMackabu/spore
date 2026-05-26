userland_dhcpc = custom_target('userland-dhcpc',
  input: files('main.c') + userland_libspore,
  output: 'dhcpc',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_dhcpc
