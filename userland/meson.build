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
userland_image_data = []

subdir('etc')
userland_image_data += files('usr/local/htoprc')
userland_image_data += files('demos/something', 'demos/something-else.msh')

userland_image_data += files('bin/deluser.sh', 'bin/hostname.sh')

subdir('lib/spore')

subdir('bin/init')
subdir('bin/msh')
subdir('prebuilt')
subdir('bin/myc')
subdir('bin/shutdown')
subdir('bin/reboot')
subdir('bin/dhcpc')
subdir('bin/logger')
subdir('libexec/trustd')
subdir('libexec/spore-test/pthread-demo')
subdir('libexec/spore-test/udp-echo')
subdir('libexec/spore-test/udp-send')
subdir('libexec/spore-test/udp-recv')
subdir('libexec/spore-test/uds-server')
subdir('libexec/spore-test/uds-client')

subdir('usr/local/bin/curl')
subdir('usr/local/bin/apk')

subdir('demos/spinner')
subdir('demos/cpuburn')
subdir('demos/peeker')
subdir('demos/writer')
subdir('demos/memhog')
subdir('demos/escalate')
subdir('demos/signal-crash')
subdir('demos/inodes')

subdir('tests/integration')
