userland_ls = custom_target('userland-ls',
  input: files('main.c') + userland_libspore,
  output: 'ls',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_ls
