userland_ssh = custom_target('userland-ssh',
  output: ['ssh', 'dbclient', 'dropbearkey', 'dropbearconvert'],
  command: [
    files('build.sh'),
    meson.project_source_root(),
    meson.current_build_dir(),
    '@OUTPUT0@',
    '@OUTPUT1@',
    '@OUTPUT2@',
    '@OUTPUT3@',
  ])
userland_image_targets += userland_ssh
