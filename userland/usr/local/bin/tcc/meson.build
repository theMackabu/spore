userland_tcc = custom_target('userland-tcc',
  output: ['tcc', 'libtcc1.a'],
  command: [
    files('build.sh'),
    meson.project_source_root(),
    meson.current_build_dir(),
    '@OUTPUT0@',
    '@OUTPUT1@',
  ])
userland_image_targets += userland_tcc
