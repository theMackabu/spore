kernel_c_args = [
  '-ffreestanding',
  '-fno-stack-protector',
  '-fno-stack-check',
  '-fno-PIC',
  '-fno-PIE',
  '-fno-lto',
  '-fno-omit-frame-pointer',
  '-fno-asynchronous-unwind-tables',
  '-mgeneral-regs-only',
  '-march=armv8-a',
  '-mcmodel=small',
  '-Wall',
  '-Wextra',
  '-Wpedantic',
  '-Wno-unused-function',
]

kernel_target_args = []
if not meson.is_cross_build()
  kernel_target_args = ['--target=aarch64-none-elf', '-Wno-unused-command-line-argument']
endif
kernel_link_args = kernel_target_args + [
  '-fuse-ld=lld',
  '-nostdlib',
  '-static',
  '-Wl,-z,max-page-size=0x1000',
  '-T', meson.current_source_dir() / 'arch/aarch64/linker.ld',
]

kernel_sources = []
subdir('fs/ext2')
subdir('fs/ramfs')
subdir('fs/vfs')
subdir('fs/procfs')
subdir('net/socket')
subdir('tty')
subdir('exec')
subdir('init')
subdir('proc')
subdir('arch/aarch64')
subdir('abi/linux/aarch64')
subdir('mm')
subdir('net')
subdir('drivers')
subdir('lib')

spore_version_conf = configuration_data()
spore_version_conf.set_quoted('SPORE_UTS_SYSNAME', spore_sysname)
spore_version_conf.set_quoted('SPORE_UTS_NODENAME', spore_nodename)
spore_version_conf.set_quoted('SPORE_UTS_RELEASE', spore_release)
spore_version_conf.set_quoted('SPORE_UTS_VERSION', spore_uts_version)
spore_version_conf.set_quoted('SPORE_UTS_MACHINE', spore_machine)
spore_version_conf.set_quoted('SPORE_UTS_DOMAINNAME', spore_domainname)
configure_file(
  output: 'spore_version.h',
  configuration: spore_version_conf)

kernel_elf = executable('kernel.elf',
  kernel_sources, limine_header,
  include_directories: [
    include_directories('.'),
    include_directories('include'),
    include_directories('../boot'),
  ],
  c_args: kernel_target_args + kernel_c_args,
  link_args: kernel_link_args,
  link_depends: files('arch/aarch64/linker.ld'),
  install: false)
