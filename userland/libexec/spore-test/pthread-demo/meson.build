userland_pthread_demo = custom_target('userland-pthread-demo',
  input: files('main.c') + userland_libspore,
  output: 'pthread-demo',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_pthread_demo
