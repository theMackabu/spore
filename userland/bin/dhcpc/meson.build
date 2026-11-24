dhcpc_archive = custom_target('dhcpc-rust',
  input: 'main.rs',
  depend_files: userland_rust_panic + files('socket.rs'),
  output: 'libdhcpc.a',
  command: userland_rust_archive + ['@INPUT@', '-o', '@OUTPUT@'])

userland_dhcpc = custom_target('userland-dhcpc',
  input: userland_libspore + [dhcpc_archive],
  output: 'dhcpc',
  command: userland_cc + ['-Wl,-z,max-page-size=4096', '@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_dhcpc
