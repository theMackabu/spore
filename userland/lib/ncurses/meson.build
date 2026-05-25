userland_ncurses = custom_target('userland-ncurses',
  output: 'libncurses.so.6.4',
  command: [
    files('build.sh'),
    meson.project_source_root(),
    meson.current_build_dir(),
    '@OUTPUT@',
  ])
userland_image_targets += userland_ncurses
