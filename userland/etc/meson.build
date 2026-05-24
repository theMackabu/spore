os_release_conf = configuration_data()
os_release_conf.set('SPORE_RELEASE', spore_release)
userland_os_release = configure_file(
  input: 'os-release.in',
  output: 'os-release',
  configuration: os_release_conf)

userland_image_data += files(
  'motd',
  'profile',
  'htoprc',
  'fastfetch/config.jsonc',
  'fastfetch/spore.logo',
) + userland_os_release
