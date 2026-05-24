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
kernel_link_args = []
if not meson.is_cross_build()
  kernel_target_args = ['--target=aarch64-none-elf']
  kernel_link_args = kernel_target_args + [
    '-fuse-ld=lld',
    '-nostdlib',
    '-static',
    '-Wl,-z,max-page-size=0x1000',
    '-T', meson.current_source_dir() / 'linker.ld',
  ]
endif

kernel_sources = files(
  'src/arch/aarch64/boot.S',
  'src/arch/aarch64/exceptions.c',
  'src/arch/aarch64/syscall.c',
  'src/arch/aarch64/timer.c',
  'src/arch/aarch64/vectors.S',
  'src/cell.c',
  'src/ext2.c',
  'src/main.c',
  'src/drivers/pl011.c',
  'src/drivers/virtio_blk.c',
  'src/drivers/virtio_console.c',
  'src/drivers/virtio_net.c',
  'src/elf/loader.c',
  'src/exec/stack.c',
  'src/lib/mem.c',
  'src/mm/pmm.c',
  'src/mm/vma.c',
  'src/mm/vmm.c',
  'src/net/net.c',
  'src/ramfs.c',
  'src/vfs.c',
)

kernel_elf = executable('kernel.elf',
  kernel_sources,
  include_directories: [
    include_directories('src/include'),
  ],
  c_args: kernel_target_args + kernel_c_args,
  link_args: kernel_link_args,
  link_depends: files('linker.ld'),
  install: false)

image_root = meson.current_build_dir() / 'image_root'
image_img = meson.project_build_root() / 'image.img'
root_ext2 = meson.project_build_root() / 'root.ext2'
test_image_root = meson.current_build_dir() / 'test_image_root'
test_image_img = meson.project_build_root() / 'test_image.img'
test_root_ext2 = meson.project_build_root() / 'test_root.ext2'

boot_efi = custom_target('BOOTAA64.EFI',
  input: files('../bootloader/main.c'),
  output: 'BOOTAA64.EFI',
  command: [
    'clang',
    '--target=aarch64-unknown-windows',
    '-ffreestanding',
    '-fshort-wchar',
    '-fno-stack-protector',
    '-fno-builtin',
    '-nostdlib',
    '-Wno-builtin-requires-header',
    '-fuse-ld=lld-link',
    '-Wl,/subsystem:efi_application',
    '-Wl,/entry:efi_main',
    '@INPUT@',
    '-o',
    '@OUTPUT@',
  ],
  build_by_default: true)

image = custom_target('image.img',
  input: [
    kernel_elf,
    files('../userland/image.manifest'),
    boot_efi,
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
    boot_efi,
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
