userland_cpuburn = custom_target('userland-cpuburn',
  input: files('main.c') + userland_libspore,
  output: 'cpuburn',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_cpuburn
