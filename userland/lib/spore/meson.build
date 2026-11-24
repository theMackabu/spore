userland_libspore = [custom_target('guest-libspore',
  input: 'util.c',
  output: 'spore.o',
  depfile: 'spore.d',
  command: userland_compiler + ['-fPIC', '-MMD', '-MF', '@DEPFILE@', '-c', '@INPUT@', '-o', '@OUTPUT@'])]
