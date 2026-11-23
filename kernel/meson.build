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

image_root = meson.current_build_dir() / 'image_root'
image_img = meson.project_build_root() / 'image.img'
root_ext2 = meson.project_build_root() / 'root.ext2'
test_image_root = meson.current_build_dir() / 'test_image_root'
test_image_img = meson.project_build_root() / 'test_image.img'
test_root_ext2 = meson.project_build_root() / 'test_root.ext2'

image = custom_target('image.img',
  input: [
    kernel_elf,
    files('../userland/image.manifest'),
    userland_image_data,
    boot_efi, limine_data,
  ],
  output: ['image.img', 'root.ext2'],
  command: [
    spore_image,
    meson.project_source_root(),
    image_root,
    kernel_elf,
    boot_efi,
    image_img,
    '@OUTPUT0@',
    root_ext2,
    '@OUTPUT1@',
    files('../userland/image.manifest'),
    meson.project_build_root(),
  ],
  depends: userland_image_targets,
  build_by_default: true)

test_image = custom_target('test_image.img',
  input: [
    kernel_elf,
    files('../userland/tests/integration/image.manifest'),
    userland_image_data,
    boot_efi, limine_data,
  ],
  output: ['test_image.img', 'test_root.ext2'],
  command: [
    spore_image,
    meson.project_source_root(),
    test_image_root,
    kernel_elf,
    boot_efi,
    test_image_img,
    '@OUTPUT0@',
    test_root_ext2,
    '@OUTPUT1@',
    files('../userland/tests/integration/image.manifest'),
    meson.project_build_root(),
  ],
  depends: userland_test_targets)
