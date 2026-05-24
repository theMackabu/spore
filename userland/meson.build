userland_musl_cc = find_program('aarch64-unknown-linux-musl-gcc')
userland_include_arg = '-I' + meson.current_source_dir() / 'lib/spore'

userland_cc = [
  userland_musl_cc,
  '-std=c23',
  '-D_GNU_SOURCE',
  '-s',
  '-Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1',
  '-Wl,-rpath,/lib',
  userland_include_arg,
]
userland_static_cc = [
  userland_musl_cc,
  '-static',
  '-std=c23',
  '-D_GNU_SOURCE',
  '-s',
  userland_include_arg,
]
userland_image_targets = []
userland_test_targets = []

subdir('lib/spore')

subdir('bin/init')
subdir('bin/sh')
subdir('bin/shutdown')
subdir('bin/ls')
subdir('bin/cat')
subdir('bin/echo')
subdir('bin/clear')
subdir('bin/file')
subdir('bin/stat')
subdir('bin/uname')
subdir('bin/edit')
subdir('bin/mkdir')
subdir('bin/rm')
subdir('bin/touch')
subdir('bin/pwd')
subdir('bin/hello')
subdir('bin/true')
subdir('bin/false')
subdir('bin/pthread-demo')
subdir('bin/ping')
subdir('bin/udp-echo')
subdir('bin/udp-send')
subdir('bin/udp-recv')

subdir('demos/spinner')
subdir('demos/peeker')
subdir('demos/writer')
subdir('demos/memhog')
subdir('demos/escalate')

subdir('tests/integration')
