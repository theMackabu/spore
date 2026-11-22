userland_image_targets += custom_target('spore-terminfo',
  input: 'terminfo/spore.src',
  output: ['xterm-256color', 'spore'],
  command: [files('terminfo.sh'), '@INPUT@', '@OUTPUT0@', '@OUTPUT1@'])
