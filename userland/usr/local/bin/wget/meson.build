userland_wget = custom_target('userland-wget',
  output: 'wget',
  command: [
    files('build.sh'),
    meson.project_source_root(),
    meson.current_build_dir(),
    meson.project_build_root(),
    '@OUTPUT@',
  ])
userland_image_targets += userland_wget
