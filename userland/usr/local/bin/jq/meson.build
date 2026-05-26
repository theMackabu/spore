userland_jq = custom_target('userland-jq',
  output: 'jq',
  command: [
    files('build.sh'),
    meson.project_source_root(),
    meson.current_build_dir(),
    '@OUTPUT@',
  ])
userland_image_targets += userland_jq
