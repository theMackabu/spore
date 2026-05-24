userland_escalate = custom_target('userland-escalate',
  input: files('main.c') + userland_libspore,
  output: 'escalate',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_escalate
