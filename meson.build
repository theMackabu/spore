project('spore', 'c', 'rust', meson_version: '>=1.10.0', default_options: [
  'buildtype=debugoptimized',
  'c_std=c23',
  'rust_std=2024',
  'build.rust_std=2024',
  'build.c_std=c23',
  'warning_level=3',
  'b_asneeded=false',
  'b_pie=false',
], version: '0.7.0')

rustc = find_program('rustc', native: true)
rust_args = ['--edition=2024', '-C', 'panic=abort']
rust_archive_args = rust_args + ['--crate-type=staticlib', '-C', 'opt-level=2', '-C', 'codegen-units=1', '-C', 'lto=fat']

spore_sysname = 'Spore'
spore_nodename = 'spore.local'
spore_machine = 'aarch64'
spore_domainname = 'Spore'

spore_release = meson.project_version()
spore_build_date = run_command('date', '+%a %b %d %Y', check: true).stdout().strip()
spore_uts_version = spore_sysname + ' Kernel Version ' + spore_release + ': ' + spore_build_date + ';'

subdir('userland')
runner_terminal = static_library('runner-terminal', 'tools/src/runner/terminal.c', native: true)
subdir('boot')
subdir('kernel')
spore_cli = executable('spore', 'tools/src/spore.rs',
  native: true,
  override_options: ['rust_std=2024'],
  rust_args: get_option('userland') ? ['--cfg', 'spore_userland'] : [],
  link_with: runner_terminal,
  install: false)
alias_target('cli', spore_cli, kernel_boot_elf, direct_kernel, boot_efi)
if get_option('userland')
  subdir('images')
  system_root_disk = custom_target(output: 'root.ext2', kwargs: rootfs['image'])
  test_root_disk = custom_target(output: 'test_root.ext2', kwargs: rootfs['test_image'])
  custom_target(output: 'image.img', depends: system_root_disk, build_by_default: true, kwargs: esp)
  custom_target(output: 'test_image.img', depends: test_root_disk, kwargs: esp)
endif

subdir('tests')
