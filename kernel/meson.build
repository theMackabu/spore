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

kernel_sources = files(
  'src/arch/aarch64/boot.S',
  'src/arch/aarch64/exceptions.c',
  'src/arch/aarch64/syscall.c',
  'src/arch/aarch64/vectors.S',
  'src/cell.c',
  'src/main.c',
  'src/drivers/pl011.c',
  'src/elf/loader.c',
  'src/exec/stack.c',
  'src/lib/mem.c',
  'src/mm/pmm.c',
  'src/mm/vmm.c',
  'src/ramfs.c',
)

init_bin = custom_target('init',
  input: files('../userland/spore_v1_demo.c'),
  output: 'init',
  command: [
    'zig', 'cc',
    '-target', 'aarch64-linux-musl',
    '-static',
    '@INPUT@',
    '-o', '@OUTPUT@',
  ],
  build_by_default: true)

kernel_elf = executable('kernel.elf',
  kernel_sources,
  include_directories: [
    include_directories('src/include'),
    include_directories('../third_party/limine-protocol/include'),
  ],
  c_args: kernel_c_args,
  link_depends: files('linker.ld'),
  install: false)

limine_dir = meson.project_source_root() / 'third_party' / 'limine'
iso_root = meson.current_build_dir() / 'iso_root'
image_iso = meson.project_build_root() / 'image.iso'

make_limine = custom_target('limine-host-tool',
  output: 'limine-built.stamp',
  command: ['sh', '-c', 'make -C "$1" && touch "$2"', 'sh', limine_dir, '@OUTPUT@'],
  build_by_default: true)

image = custom_target('image.iso',
  input: [kernel_elf, files('boot/limine.conf'), make_limine, init_bin],
  output: 'image.iso',
  command: [
    'sh', '-c',
    'set -eu; ' +
    'rm -rf "$1"; ' +
    'mkdir -p "$1/boot/limine" "$1/EFI/BOOT"; ' +
    'cp "$2" "$1/boot/kernel.elf"; ' +
    'cp "$3" "$1/boot/limine/limine.conf"; ' +
    'cp "$3" "$1/EFI/BOOT/limine.conf"; ' +
    'cp "$4/BOOTAA64.EFI" "$1/EFI/BOOT/BOOTAA64.EFI"; ' +
    'cp "$5" "$1/boot/init"; ' +
    'efi_img="$1/efi.img"; ' +
    'rm -f "$efi_img"; ' +
    'dd if=/dev/zero of="$efi_img" bs=1M count=64 >/dev/null 2>&1; ' +
    'mkfs.fat -F 32 "$efi_img" >/dev/null; ' +
    'mmd -i "$efi_img" ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine; ' +
    'mcopy -i "$efi_img" "$4/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI; ' +
    'mcopy -i "$efi_img" "$3" ::/EFI/BOOT/limine.conf; ' +
    'mcopy -i "$efi_img" "$3" ::/boot/limine/limine.conf; ' +
    'mcopy -i "$efi_img" "$2" ::/boot/kernel.elf; ' +
    'mcopy -i "$efi_img" "$5" ::/boot/init; ' +
    'xorriso -as mkisofs ' +
    '-R -r -J -hfsplus -apm-block-size 2048 ' +
    '--efi-boot efi.img -no-emul-boot -efi-boot-part --efi-boot-image ' +
    '--protective-msdos-label "$1" -o "$6" >/dev/null 2>&1; ' +
    'cp "$6" "$7"',
    'sh',
    iso_root,
    kernel_elf,
    files('boot/limine.conf'),
    limine_dir,
    init_bin,
    image_iso,
    '@OUTPUT@',
  ],
  build_by_default: true)

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

qemu_args = [
  'qemu-system-aarch64',
  '-M', 'virt',
  '-accel', 'hvf',
  '-cpu', 'host',
  '-m', '512M',
  '-boot', 'order=d,menu=off,strict=on',
  '-drive', 'if=pflash,format=raw,readonly=on,file=' + qemu_code,
  '-cdrom', image_iso,
  '-serial', 'stdio',
  '-display', 'none',
]

run_target('run',
  command: [
    files('../tools/run_qemu.py'),
  ] + qemu_args,
  depends: image)

run_target('run-raw',
  command: qemu_args,
  depends: image)

run_target('run-tcg',
  command: [
    files('../tools/run_qemu.py'),
    'qemu-system-aarch64',
    '-M', 'virt',
    '-accel', 'tcg',
    '-cpu', 'max',
    '-m', '512M',
    '-boot', 'order=d,menu=off,strict=on',
    '-drive', 'if=pflash,format=raw,readonly=on,file=' + qemu_code,
    '-cdrom', image_iso,
    '-serial', 'stdio',
    '-display', 'none',
  ],
  depends: image)
