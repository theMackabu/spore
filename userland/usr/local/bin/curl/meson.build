userland_curl = custom_target('userland-curl',
  output: 'curl',
  command: [
    files('build.sh'),
    meson.project_source_root(),
    meson.current_build_dir(),
    meson.project_build_root(),
    '@OUTPUT@',
  ],
  depends: userland_mbedtls)
userland_image_targets += userland_curl
