userland_musl_cc = find_program('aarch64-unknown-linux-musl-gcc')
guest_rust_target = '--target=aarch64-unknown-linux-musl'
userland_rust_archive = [rustc, rust_archive_args, guest_rust_target, '-C', 'relocation-model=pic']
userland_rust_panic = files('lib/spore/panic.rs')
userland_include_arg = '-I' + meson.current_source_dir() / 'lib/spore'

userland_compiler = [userland_musl_cc, '-std=c23', '-D_GNU_SOURCE', '-s', '-ffunction-sections', '-fdata-sections', userland_include_arg]
optimization = get_option('optimization')
if optimization != 'plain'
  userland_compiler += '-O' + optimization
endif
userland_link = userland_compiler + ['-Wl,--gc-sections,-z,max-page-size=4096']
userland_cc = userland_link + [
  '-Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1',
  '-Wl,-rpath,/lib',
]
userland_static_cc = userland_link + ['-static']
userland_image_targets = []
userland_test_targets = []

subdir('etc')

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
