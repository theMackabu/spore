userland_vim = custom_target('userland-vim',
  output: 'vim',
  depends: userland_ncurses,
  command: [
    files('build.sh'),
    meson.project_source_root(),
    meson.current_build_dir(),
    '@OUTPUT@',
  ])
userland_image_targets += userland_vim
