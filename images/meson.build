fs = import('fs')
source_root = meson.project_source_root()
build_root = meson.project_build_root()

esp = {
  'input': [kernel_boot_elf, boot_efi, limine_data],
  'command': [spore_cli, '_image', 'esp', '@INPUT@', '@OUTPUT@'],
}

rootfs = {}
foreach name, variant : {
  'image': ['../userland/image.manifest', userland_image_targets],
  'test_image': ['../userland/tests/integration/image.manifest', userland_test_targets],
}
  manifest = variant[0]
  inputs = files(manifest) + [userland_os_release]
  foreach line : fs.read(manifest).split('\n')
    fields = line.strip().split()
    if fields.length() == 2 and not fields[0].startswith('#') and fs.is_file(source_root / fields[0])
      inputs += files('..' / fields[0])
    endif
  endforeach
  rootfs += {name: {
    'input': inputs,
    'depends': variant[1],
    'command': [spore_cli, '_image', 'rootfs', source_root, build_root / (name + '_root'),
                '@INPUT0@', build_root, '@OUTPUT@'],
  }}
endforeach
