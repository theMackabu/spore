unpack = files('../userland/prebuilt/unpack.py')
boot_efi = custom_target('limine-efi',
  output: 'BOOTAA64.EFI',
  command: [python, unpack, cache_dir,
    'https://github.com/Limine-Bootloader/Limine/releases/download/v12.11.1/limine-binary.zip',
    '27773b8b974f6ab55d7be5bd15a6914fe70a91f0c56858ca0cbe76c590b60a22',
    meson.current_build_dir(), 'limine-binary/BOOTAA64.EFI'])
limine_data = files('limine.conf', 'LIMINE-LICENSE')

protocol_rev = '877c2d4c50e170575f8b8e7fbf7b8fa89b1dc157'
limine_header = custom_target('limine-protocol',
  output: 'limine.h',
  command: [python, unpack, cache_dir,
    'https://github.com/Limine-Bootloader/limine-protocol/archive/' + protocol_rev + '.zip',
    'f792c62257377208e6f53a31aa3442eb182e9ba451f139749296da9d411f2adb',
    meson.current_build_dir(), 'limine-protocol-' + protocol_rev + '/include/limine.h'])
