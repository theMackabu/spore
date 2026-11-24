userland_image_targets = []
userland_test_targets = []
subdir('prebuilt')
if not get_option('userland')
  subdir_done()
endif

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

subdir('etc')
subdir('lib/spore')

foreach program : [
  'bin/logger',
  'bin/myc',
  'bin/reboot',
  'bin/shutdown',
  'demos/cpuburn',
  'demos/escalate',
  'demos/inodes',
  'demos/memhog',
  'demos/peeker',
  'demos/signal-crash',
  'demos/spinner',
  'demos/writer',
  'libexec/spore-test/pthread-demo',
  'libexec/spore-test/udp-echo',
  'libexec/spore-test/udp-recv',
  'libexec/spore-test/udp-send',
  'libexec/spore-test/uds-client',
  'libexec/spore-test/uds-server',
]
  name = program.split('/')[-1]
  userland_image_targets += custom_target('userland-' + name,
    input: files(program / 'main.c') + userland_libspore,
    build_subdir: 'programs',
    output: name,
    command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
endforeach

subdir('bin/init')
subdir('bin/msh')
subdir('bin/dhcpc')
subdir('libexec/trustd')

subdir('usr/local/bin/curl')
subdir('usr/local/bin/apk')

subdir('tests/integration')
