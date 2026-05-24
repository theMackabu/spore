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
  'src/main.c',
  'src/drivers/pl011.c',
  'src/drivers/virtio_net.c',
  'src/elf/loader.c',
  'src/exec/stack.c',
  'src/lib/mem.c',
  'src/mm/pmm.c',
  'src/mm/vma.c',
  'src/mm/vmm.c',
  'src/ramfs.c',
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

iso_root = meson.current_build_dir() / 'iso_root'
image_iso = meson.project_build_root() / 'image.iso'
test_iso_root = meson.current_build_dir() / 'test_iso_root'
test_image_iso = meson.project_build_root() / 'test_image.iso'

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

image = custom_target('image.iso',
  input: [
    kernel_elf,
    files('../userland/image.manifest'),
    files('../tools/build_image.py'),
    boot_efi,
  ],
  output: 'image.iso',
  command: [
    files('../tools/build_image.py'),
    meson.project_source_root(),
    iso_root,
    kernel_elf,
    boot_efi,
    image_iso,
    '@OUTPUT@',
    files('../userland/image.manifest'),
    meson.project_build_root(),
  ],
  depends: userland_image_targets,
  build_always_stale: true,
  build_by_default: true)

test_image = custom_target('test_image.iso',
  input: [
    kernel_elf,
    files('../userland/tests/integration/image.manifest'),
    files('../tools/build_image.py'),
    boot_efi,
  ],
  output: 'test_image.iso',
  command: [
    files('../tools/build_image.py'),
    meson.project_source_root(),
    test_iso_root,
    kernel_elf,
    boot_efi,
    test_image_iso,
    '@OUTPUT@',
    files('../userland/tests/integration/image.manifest'),
    meson.project_build_root(),
  ],
  depends: userland_test_targets,
  build_always_stale: true)

qemu_code = run_command('sh', '-c',
  'for dir in $(qemu-system-aarch64 -L help 2>/dev/null); do ' +
  '  for file in "$dir/edk2-aarch64-code.fd" "${dir%-firmware}/qemu/edk2-aarch64-code.fd"; do ' +
  '    if [ -f "$file" ]; then printf "%s" "$file"; exit 0; fi; ' +
  '  done; ' +
  'done; ' +
  'if [ -f /opt/homebrew/opt/qemu/share/qemu/edk2-aarch64-code.fd ]; then ' +
  '  printf "%s" /opt/homebrew/opt/qemu/share/qemu/edk2-aarch64-code.fd; exit 0; ' +
  'fi; ' +
  'exit 1',
  check: true).stdout().strip()

qemu_base_args = [
  'qemu-system-aarch64',
  '-M', 'virt,gic-version=3',
  '-accel', 'hvf',
  '-cpu', 'host',
  '-m', '512M',
  '-global', 'virtio-mmio.force-legacy=false',
  '-netdev', 'user,id=sporenet',
  '-device', 'virtio-net-device,netdev=sporenet,mac=52:54:00:12:34:56',
  '-boot', 'order=d,menu=off,strict=on',
  '-drive', 'if=pflash,format=raw,readonly=on,file=' + qemu_code,
]

qemu_args = qemu_base_args + [
  '-cdrom', image_iso,
  '-serial', 'stdio',
  '-display', 'none',
]

test_qemu_args = qemu_base_args + [
  '-cdrom', test_image_iso,
  '-serial', 'stdio',
  '-display', 'none',
]

run_target('run',
  command: [
    files('../tools/run_qemu.py'),
    '--mode', 'plain',
  ] + qemu_args,
  depends: image)

run_target('run-tests',
  command: [
    files('../tools/run_qemu.py'),
    '--mode', 'filter',
  ] + test_qemu_args,
  depends: test_image)
