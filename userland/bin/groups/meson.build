userland_groups = custom_target('userland-groups',
  input: files('main.c') + userland_libspore,
  output: 'groups',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_groups
