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
alias_target('cli', spore_cli, kernel_boot_elf, boot_efi)
if get_option('userland')
  fs = import('fs')
  foreach variant : [
    ['image', 'root', 'userland/image.manifest', userland_image_targets],
    ['test_image', 'test_root', 'userland/tests/integration/image.manifest', userland_test_targets],
  ]
    manifest_data = [userland_os_release]
    foreach line : fs.read(variant[2]).split('\n')
      fields = line.strip().split()
      if fields.length() == 2 and not fields[0].startswith('#') and fs.is_file(fields[0])
        manifest_data += files(fields[0])
      endif
    endforeach
    root_disk = custom_target(variant[1] + '.ext2',
      input: [files(variant[2]), manifest_data],
      output: variant[1] + '.ext2',
      command: [spore_cli, '_image', 'rootfs', meson.project_source_root(),
        meson.project_build_root() / (variant[0] + '_root'), '@INPUT0@',
        meson.project_build_root(), '@OUTPUT@'],
      depends: variant[3])
    if variant[1] == 'root'
      system_root_disk = root_disk
    endif
    custom_target(variant[0] + '.img',
      input: [kernel_boot_elf, boot_efi, limine_data],
      output: variant[0] + '.img',
      command: [spore_cli, '_image', 'esp', '@INPUT@', '@OUTPUT@'],
      depends: root_disk,
      build_by_default: variant[0] == 'image')
  endforeach
endif

subdir('tests')
