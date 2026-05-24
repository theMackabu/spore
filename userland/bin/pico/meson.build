userland_pico = custom_target('userland-pico',
  input: files('main.c', 'buffer.c', 'file.c', 'screen.c', 'term.c', 'pico.h') + userland_libspore,
  output: 'pico',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_pico
