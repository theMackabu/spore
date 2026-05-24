userland_integration = custom_target('userland-integration',
  input: files('main.c') + userland_libspore,
  output: 'integration',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_exec_child = custom_target('userland-exec-child',
  input: files('exec_child.c'),
  output: 'exec_child',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_test_targets += [userland_integration, userland_exec_child]
