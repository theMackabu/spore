userland_integration = custom_target('userland-integration',
  input: files(
    'main.c',
    'process.c',
    'socket.c',
    'policy.c',
    'signal.c',
    'memory.c',
    'filesystem.c',
  ) + userland_libspore,
  output: 'integration',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'],
  depend_files: files('dirent64.h', 'integration.h', 'integration_socket.h'))

userland_exec_child = custom_target('userland-exec-child',
  input: files('exec_child.c'),
  output: 'exec_child',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])

userland_test_targets += [userland_integration, userland_exec_child]
