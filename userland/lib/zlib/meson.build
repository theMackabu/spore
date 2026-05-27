userland_zlib = custom_target('userland-zlib',
  output: 'zlib.stamp',
  command: [
    files('build.sh'),
    meson.project_source_root(),
    meson.current_build_dir(),
    '@OUTPUT@',
  ])
