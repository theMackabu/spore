userland_dhcpc = custom_target('userland-dhcpc',
  input: files('main.rs'),
  output: 'dhcpc',
  command: userland_rustc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_dhcpc
