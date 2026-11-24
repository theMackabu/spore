units_archive = custom_target('init-units-rust',
  input: 'units.rs',
  depend_files: userland_rust_panic,
  output: 'libinit_units.a',
  command: userland_rust_archive + ['@INPUT@', '-o', '@OUTPUT@'])

userland_init = custom_target('userland-init',
  input: files(
    'main.c',
    'common.c',
    'ui.c',
    'units.c',
    'service.c',
    'control.c',
  ) + userland_libspore + [units_archive],
  depend_files: files('mycelium.h'),
  output: 'init',
  command: userland_static_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_init
