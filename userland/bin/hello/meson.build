userland_hello = custom_target('userland-hello',
  input: files('main.c') + userland_libspore,
  output: 'hello',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_hello
