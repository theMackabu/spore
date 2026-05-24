userland_signal_crash = custom_target('userland-signal-crash',
  input: files('main.c') + userland_libspore,
  output: 'signal-crash',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_signal_crash
