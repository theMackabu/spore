userland_wget = custom_target('userland-wget',
  output: ['wget', 'which'],
  command: [
    files('build.sh'),
    meson.project_source_root(),
    meson.current_build_dir(),
    meson.project_build_root(),
    '@OUTPUT0@',
    '@OUTPUT1@',
  ])
userland_image_targets += userland_wget
