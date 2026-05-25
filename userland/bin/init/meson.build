userland_init = custom_target('userland-init',
  input: files(
    'main.c',
    'common.c',
    'units.c',
    'service.c',
    'control.c',
  ) + userland_libspore,
  output: 'init',
  command: userland_static_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_init
