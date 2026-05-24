userland_zig = find_program('zig')
userland_include_arg = '-I' + meson.current_source_dir() / 'lib/spore'
userland_cc = [
  userland_zig,
  'cc',
  '-target', 'aarch64-linux-musl',
  '-static',
  '-std=c23',
  '-D_GNU_SOURCE',
  userland_include_arg,
]
userland_image_targets = []
userland_test_targets = []

subdir('lib/spore')

subdir('bin/spsh')
subdir('bin/ls')
subdir('bin/cat')
subdir('bin/echo')
subdir('bin/mkdir')
subdir('bin/rm')
subdir('bin/touch')
subdir('bin/pwd')
subdir('bin/hello')
subdir('bin/true')
subdir('bin/false')
subdir('bin/pthread-demo')
subdir('bin/udp-echo')
subdir('bin/udp-send')
subdir('bin/udp-recv')

subdir('demos/spinner')
subdir('demos/peeker')
subdir('demos/writer')
subdir('demos/memhog')
subdir('demos/escalate')

subdir('tests/integration')
