userland_mbedtls = custom_target('userland-mbedtls',
  output: 'mbedtls.stamp',
  command: [
    files('build.sh'),
    meson.project_source_root(),
    meson.current_build_dir(),
    '@OUTPUT@',
  ])

